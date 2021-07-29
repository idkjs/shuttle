open! Core;
open! Async;
open Import;
module Unix = Core.Unix;

module Config = {
  /* Same as the default value of [buffer_age_limit] for [Async_unix.Writer] */
  let default_write_timeout = Time_ns.Span.of_min(2.);
  let default_max_buffer_size = Int.max_value;
  let default_initial_buffer_size = 64 * 1024;

  [@deriving sexp_of]
  type t = {
    initial_buffer_size: int,
    max_buffer_size: int,
    write_timeout: Time_ns.Span.t,
  };

  let validate = t => {
    if (t.initial_buffer_size <= 0
        || t.initial_buffer_size > t.max_buffer_size
        || Time_ns.Span.(<=)(t.write_timeout, Time_ns.Span.zero)) {
      raise_s(
        [%sexp ("Shuttle.Config.validate: invalid config", {t: (t: t)})],
      );
    };
    t;
  };

  let create =
      (
        ~initial_buffer_size=default_initial_buffer_size,
        ~max_buffer_size=default_max_buffer_size,
        ~write_timeout=default_write_timeout,
        (),
      ) =>
    validate({initial_buffer_size, max_buffer_size, write_timeout});
};

[@deriving (sexp_of, fields)]
type t = {
  fd: Fd.t,
  config: Config.t,
  mutable buf: [@sexp.opaque] Faraday.t,
  monitor: Monitor.t,
  mutable close_state: [ | `Open | `Start_close | `Closed],
  close_started: Ivar.t(unit),
  close_finished: Ivar.t(unit),
  mutable writer_state: [ | `Active | `Stopped | `Inactive],
};

let create =
    (~initial_buffer_size=?, ~max_buffer_size=?, ~write_timeout=?, fd) => {
  let config =
    Config.create(
      ~initial_buffer_size?,
      ~max_buffer_size?,
      ~write_timeout?,
      (),
    );
  set_nonblock(fd);
  {
    fd,
    config,
    writer_state: `Inactive,
    buf: Faraday.create(config.initial_buffer_size),
    monitor: Monitor.create(),
    close_state: `Open,
    close_started: Ivar.create(),
    close_finished: Ivar.create(),
  };
};

let is_closed = t =>
  switch (t.close_state) {
  | `Open => false
  | `Closed
  | `Start_close => true
  };

let close_started = t => Ivar.read(t.close_started);
let close_finished = t => Ivar.read(t.close_finished);
let is_open = Fn.non(is_closed);

let mk_iovecs = iovecs =>
  Array.of_list_map(iovecs, ~f=({Faraday.buffer, off, len}) =>
    Unix.IOVec.of_bigstring(buffer, ~pos=off, ~len)
  );

let write_iovecs = (t, iovecs) =>
  switch (
    Bigstring_unix.writev_assume_fd_is_nonblocking(
      Fd.file_descr_exn(t.fd),
      iovecs,
    )
  ) {
  | n =>
    Faraday.shift(t.buf, n);
    `Ok;
  | exception (
                [@implicit_arity]
                Unix.Unix_error(EWOULDBLOCK | EAGAIN | EINTR, _, _)
              ) => `Ok
  | exception (
                [@implicit_arity]
                Unix.Unix_error(
                  EPIPE | ECONNRESET | EHOSTUNREACH | ENETDOWN | ENETRESET |
                  ENETUNREACH |
                  ETIMEDOUT,
                  _,
                  _,
                )
              ) => `Eof
  | exception exn => raise(exn)
  };

let flushed = (t, f) => Faraday.flush(t.buf, f);

let close = t => {
  switch (t.close_state) {
  | `Closed
  | `Start_close => ()
  | `Open =>
    t.close_state = `Start_close;
    Ivar.fill(t.close_started, ());
    Deferred.any_unit([
      after(Time.Span.of_sec(5.)),
      Deferred.create(ivar => flushed(t, () => Ivar.fill(ivar, ()))),
    ])
    >>> (
      () => {
        t.close_state = `Closed;
        Fd.close(t.fd) >>> (() => Ivar.fill(t.close_finished, ()));
      }
    );
  };
  close_finished(t);
};

let stop_writer = t => {
  t.writer_state = `Stopped;
  ignore(Faraday.drain(t.buf): int);
};

module Single_write_result = {
  type t =
    | Continue
    | Stop;
};

let single_write = t =>
  switch (Faraday.operation(t.buf)) {
  | `Yield => Single_write_result.Continue
  | `Close => Stop
  | `Writev(iovecs) =>
    switch (write_iovecs(t, mk_iovecs(iovecs))) {
    | `Ok => Continue
    | `Eof => Stop
    }
  };

let rec write_everything = t =>
  switch (single_write(t)) {
  | Stop => stop_writer(t)
  | Continue =>
    if (!Faraday.has_pending_output(t.buf)) {
      t.writer_state = `Inactive;
      if (is_closed(t)) {
        stop_writer(t);
      };
    } else {
      wait_and_write_everything(t);
    }
  }

and wait_and_write_everything = t =>
  Clock_ns.with_timeout(t.config.write_timeout, Fd.ready_to(t.fd, `Write))
  >>> (
    result =>
      switch (result) {
      | `Result(`Ready) => write_everything(t)
      | `Timeout =>
        Log.Global.sexp(
          ~level=`Error,
          [%message
            "Async_transport.Writer timed out waiting to write on file descriptor. Closing the writer."(
              ~timeout=t.config.write_timeout: Time_ns.Span.t,
              t: t,
            )
          ],
        );
        stop_writer(t);
      | `Result((`Bad_fd | `Closed) as result) =>
        raise_s(
          [%sexp
            (
              "Async_transport.Writer: fd changed",
              {t: (t: t), ready_to_result: (result: [ | `Bad_fd | `Closed])},
            )
          ],
        )
      }
  );

let is_writing = t =>
  switch (t.writer_state) {
  | `Active => true
  | `Inactive => false
  | `Stopped => false
  };

let flush = t =>
  if (!is_writing(t) && Faraday.has_pending_output(t.buf)) {
    t.writer_state = `Active;
    Scheduler.within(~monitor=t.monitor, () => write_everything(t));
  };

let ensure_can_write = t =>
  switch (t.writer_state) {
  | `Inactive
  | `Active => ()
  | `Stopped =>
    raise_s([%sexp ("Attempting to write to a closed writer", {t: (t: t)})])
  };

let schedule_bigstring = (t, ~pos=?, ~len=?, buf) => {
  ensure_can_write(t);
  Faraday.schedule_bigstring(t.buf, buf, ~off=?pos, ~len?);
};

let write_string = (t, ~pos=?, ~len=?, buf) => {
  ensure_can_write(t);
  Faraday.write_string(t.buf, buf, ~off=?pos, ~len?);
};
