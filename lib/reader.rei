/** Alternative to
    {{:https://github.com/janestreet/async_unix/blob/cdd9aba67eec2f30bb3a7a22f92c056742073726/src/reader.mli}
    Async_unix.Reader}, based on the low latency transport in async_rpc. */;

open! Core;
open! Async;

module Read_chunk_result: {
  [@deriving sexp_of]
  type t('a) =
    | /** [Stop a] indicates that the read loop's handler consumed 0 bytes and that the
            read loop should stop. */
      Stop(
        'a,
      )
    | /** [Continue] indicates that the read loop's handler consumed all bytes. */
      Continue
    | /** [Consumed count] indicates that the read loop's handler consumed [count]
            bytes. */
      Consumed(
        int,
      );
};

[@deriving sexp_of]
type t;

let create: (~buf_len: int=?, Fd.t) => t;
let is_closed: t => bool;
let close: t => Deferred.t(unit);

/** [read_one_chunk_at_a_time ~on_chunk] reads bytes into the reader's internal buffer,
    and calls [on_chunk] whenever there is data available. */

let read_one_chunk_at_a_time:
  (
    t,
    ~on_chunk: (Bigstring.t, ~pos: int, ~len: int) => Read_chunk_result.t('a)
  ) =>
  Deferred.Result.t('a, [> | `Eof | `Closed]);
