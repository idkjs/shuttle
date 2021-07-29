open Core;
open Async;

let set_nonblock = fd =>
  Fd.with_file_descr_exn(fd, ignore, ~nonblocking=true);

module Read_chunk_result = {
  [@deriving sexp_of]
  type t('a) =
    | Stop('a)
    | Continue
    | Consumed(int);
};

[@deriving sexp_of]
type t = {
  fd: Fd.t,
  mutable reading: bool,
  mutable is_closed: bool,
  closed: Ivar.t(unit),
  mutable buf: [@sexp.opaque] Bigstring.t,
  mutable pos: int,
  mutable max: int,
};

let create = (~buf_len=64 * 1024, fd) => {
  let buf_len =
    if (buf_len > 0) {
      buf_len;
    } else {
      raise_s(
        [%message
          "Reader.create got negative buf_len"(buf_len: int, fd: Fd.t)
        ],
      );
    };

  set_nonblock(fd);
  {
    fd,
    reading: false,
    is_closed: false,
    closed: Ivar.create(),
    buf: Bigstring.create(buf_len),
    pos: 0,
    max: 0,
  };
};

let is_closed = t => t.is_closed;
let closed = t => Ivar.read(t.closed);

let close = t => {
  if (!t.is_closed) {
    t.is_closed = true;
    Fd.close(t.fd) >>> (() => Ivar.fill(t.closed, ()));
  };
  closed(t);
};

let shift = t =>
  if (t.pos > 0) {
    let len = t.max - t.pos;
    if (len > 0) {
      Bigstring.blit(
        ~src=t.buf,
        ~dst=t.buf,
        ~src_pos=t.pos,
        ~dst_pos=0,
        ~len,
      );
    };
    t.pos = 0;
    t.max = len;
  };

let refill = t => {
  shift(t);
  let result =
    Bigstring_unix.read_assume_fd_is_nonblocking(
      Fd.file_descr_exn(t.fd),
      t.buf,
      ~pos=t.max,
      ~len=Bigstring.length(t.buf) - t.max,
    );

  if (Unix.Syscall_result.Int.is_ok(result)) {
    switch (Unix.Syscall_result.Int.ok_exn(result)) {
    | 0 => `Eof
    | n =>
      assert(n > 0);
      t.max = t.max + n;
      `Read_some;
    };
  } else {
    switch (Unix.Syscall_result.Int.error_exn(result)) {
    | EAGAIN
    | EWOULDBLOCK
    | EINTR => `Nothing_available
    | EPIPE
    | ECONNRESET
    | EHOSTUNREACH
    | ENETDOWN
    | ENETRESET
    | ENETUNREACH
    | ETIMEDOUT => `Eof
    | error => raise([@implicit_arity] Unix.Unix_error(error, "read", ""))
    };
  };
};

module Driver = {
  type state('a) =
    | Running
    | Stopped(stop_reason('a))

  and stop_reason('a) =
    | Handler_raised
    | Eof_reached
    | Stopped_by_user('a);

  type nonrec t('a) = {
    reader: t,
    on_chunk: (Bigstring.t, ~pos: int, ~len: int) => Read_chunk_result.t('a),
    interrupt: Ivar.t(unit),
    mutable state: state('a),
  };

  let is_running = t =>
    switch (t.state) {
    | Running => true
    | Stopped(_) => false
    };

  let interrupt = (t, reason) => {
    assert(is_running(t));
    t.state = Stopped(reason);
    Ivar.fill(t.interrupt, ());
  };

  let can_process_chunk = t => !t.reader.is_closed && is_running(t);

  let rec process_chunks = t =>
    if (can_process_chunk(t)) {
      let len = t.reader.max - t.reader.pos;
      if (len > 0) {
        switch (t.on_chunk(t.reader.buf, ~pos=t.reader.pos, ~len)) {
        | Stop(x) => interrupt(t, Stopped_by_user(x))
        | Continue => t.reader.pos = t.reader.pos + len
        | Consumed(d) =>
          if (d > len || d < 0) {
            raise_s(
              [%message
                "on_chunk returned an invalid value for consumed bytes"(
                  len: int,
                  ~consumed=d: int,
                )
              ],
            );
          };
          t.reader.pos = t.reader.pos + d;
          process_chunks(t);
        };
      };
    };

  let process_incoming = t =>
    if (can_process_chunk(t)) {
      switch (refill(t.reader)) {
      | `Eof => interrupt(t, Eof_reached)
      | `Nothing_available => ()
      | `Read_some => process_chunks(t)
      };
    };

  let stop_watching_on_error = (t, ~monitor) => {
    let parent = Monitor.current();
    Monitor.detach_and_iter_errors(
      monitor,
      ~f=exn => {
        if (is_running(t)) {
          interrupt(t, Handler_raised);
        };
        Monitor.send_exn(parent, exn);
      },
    );
  };

  let run = (reader, ~on_chunk) => {
    let t = {reader, interrupt: Ivar.create(), state: Running, on_chunk};
    let monitor =
      Monitor.create(
        ~here=[%here],
        ~name="Async_transport.Reader.Driver.run",
        (),
      );

    stop_watching_on_error(t, ~monitor);
    switch%bind (
      Scheduler.within'(
        ~monitor,
        () => {
          let interrupt =
            Deferred.any_unit([Ivar.read(t.interrupt), closed(t.reader)]);
          Fd.interruptible_every_ready_to(
            ~interrupt,
            t.reader.fd,
            `Read,
            process_incoming,
            t,
          );
        },
      )
    ) {
    | `Bad_fd
    | `Unsupported =>
      raise_s(
        [%message
          "Async_transport.Reader.run: fd doesn't support watching"(
            ~fd=t.reader.fd: Fd.t,
          )
        ],
      )
    | `Closed
    | `Interrupted =>
      switch (t.state) {
      | Running =>
        assert(Fd.is_closed(t.reader.fd) || t.reader.is_closed);
        return(Error(`Closed));
      | Stopped(Stopped_by_user(x)) => return(Ok(x))
      | Stopped(Handler_raised) => Deferred.never()
      | Stopped(Eof_reached) => return(Error(`Eof))
      }
    };
  };
};

let read_one_chunk_at_a_time = (t, ~on_chunk) => Driver.run(t, ~on_chunk);
