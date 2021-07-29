open! Core;
open! Async;

module Codec = {
  open Shuttle;

  type t = {
    writer: Writer.t,
    reader: Reader.t,
  };

  let create = fd => {
    let reader = Reader.create(fd);
    let writer = Writer.create(fd);
    {writer, reader};
  };

  let buf' = Bigstring.of_string("+PONG\r\n");

  let run_loop = t =>
    Reader.read_one_chunk_at_a_time(
      t.reader,
      ~on_chunk=(buf, ~pos, ~len) => {
        for (i in pos to len - 1) {
          if (Char.(Bigstring.get(buf, i) == '\n')) {
            Shuttle.Writer.schedule_bigstring(t.writer, buf');
          };
        };
        Writer.flush(t.writer);
        Reader.Read_chunk_result.Continue;
      },
    )
    >>| (
      fun
      | Error(`Eof) => ()
      | Error(`Closed) =>
        raise_s([%message "Attempting to read from a closed fd"])
      | Ok(_) => ()
    );
};

let run = (~port) => {
  let host_and_port =
    Tcp.Server.create_sock_inet(
      ~on_handler_error=`Raise,
      Tcp.Where_to_listen.of_port(port),
      (_addr, sock) => {
        let conn = Codec.create(Socket.fd(sock));
        Codec.run_loop(conn);
      },
    );

  ignore(host_and_port: Tcp.Server.t(Socket.Address.Inet.t, int));
  Deferred.never();
};

let () =
  Command.async(
    ~summary="Start an echo server",
    {
      open Command.Let_syntax;
      let%map_open port =
        flag(
          "-port",
          optional_with_default(8888, int),
          ~doc=" Port to listen on (default 8888)",
        );

      () => run(~port);
    },
  )
  |> Command.run;
