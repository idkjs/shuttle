open Core;
open Async;

let run = (~port) => {
  let host_and_port =
    Tcp.Server.create(
      ~on_handler_error=`Raise,
      Tcp.Where_to_listen.of_port(port),
      (_addr, r, w) => {
        let lines = Reader.lines(r);
        Pipe.iter_without_pushback(lines, ~f=_ =>
          Writer.write(w, "+PONG\r\n")
        );
      },
    );

  ignore(
    host_and_port: Deferred.t(Tcp.Server.t(Socket.Address.Inet.t, int)),
  );
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
