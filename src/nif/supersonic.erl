%% @doc SuperSonic NIF — audio engine interface for BEAM languages.
%%
%% Copyright (c) 2025 Sam Aaron. Licensed under MIT — see LICENSE.
%%
%% Wraps the SuperSonic scsynth audio engine as a NIF.  OSC messages
%% go in via {@link send_osc/1}, replies come back as Erlang messages
%% to a registered process (see {@link set_notification_pid/0}).
%%
%% Usage from Elixir:
%%   :supersonic.start(%{headless: true})
%%   :supersonic.set_notification_pid()
%%   :supersonic.send_osc(osc_binary)
%%   receive do
%%     {:osc_reply, binary} -> ...
%%   end
%%   :supersonic.stop()
%%
%% == Hot upgrade not supported ==
%%
%% Hot upgrade of this NIF is not supported. `appup' files involving
%% this module must trigger a full VM restart, not a hot reload. The
%% NIF is loaded with NULL `reload' and `upgrade' callbacks in
%% `ERL_NIF_INIT', so the BEAM rejects hot-upgrade attempts rather
%% than silently accepting them.
%%
%% Calling {@link start/1} and {@link stop/0} repeatedly within a
%% single VM lifetime is supported and tested.
%%
%% Background: the engine spins up a JUCE runtime and, on macOS, an
%% NSApplication / message thread. Their teardown can only run on
%% specific threads; calling `shutdownJuce_GUI()' from an arbitrary
%% BEAM scheduler aborts on macOS.
-module(supersonic).

-export([
    is_nif_loaded/0,
    start/1,
    stop/0,
    send_osc/1,
    set_notification_pid/0,
    clear_notification_pid/0
]).

-on_load(init/0).

%% @private Load the NIF shared library.
%% Checks SUPERSONIC_NIF_PATH env var first (for testing),
%% then falls back to the application's priv directory.
init() ->
    Path = case os:getenv("SUPERSONIC_NIF_PATH") of
        false ->
            case code:priv_dir(supersonic) of
                {error, _} ->
                    %% Not running as an OTP app — try relative to beam file
                    Dir = filename:dirname(code:which(?MODULE)),
                    filename:join(Dir, "supersonic");
                PrivDir ->
                    filename:join(PrivDir, "supersonic")
            end;
        NifDir ->
            filename:join(NifDir, "supersonic")
    end,
    erlang:load_nif(Path, 0).

%% @doc Returns `true' if the NIF is loaded, `false' otherwise.
-spec is_nif_loaded() -> true | false.
is_nif_loaded() -> false.

%% @doc Boot the audio engine.
%%
%% Config is a map with optional keys:
%%   sample_rate, num_output_channels, num_input_channels,
%%   buffer_size, num_buffers, max_nodes, num_audio_bus_channels,
%%   num_control_bus_channels, max_wire_bufs, max_graph_defs,
%%   real_time_memory_size, num_rgens, headless.
%%
%% `headless => true' skips audio device init (for testing/CI).
%%
%% Runs on a dirty I/O scheduler (heavy init, won't block normal schedulers).
-spec start(Config :: map()) -> ok | {error, term()}.
start(_Config) -> erlang:nif_error(nif_not_loaded).

%% @doc Shut down the audio engine.
-spec stop() -> ok.
stop() -> erlang:nif_error(nif_not_loaded).

%% @doc Send a raw OSC binary message to the engine.
-spec send_osc(binary()) -> ok | {error, term()}.
send_osc(_OscBinary) -> erlang:nif_error(nif_not_loaded).

%% @doc Register the calling process to receive OSC replies.
%%
%% The registered process will receive messages:
%%   {osc_reply, Binary} — OSC reply data
%%   {debug, String}     — debug output from scsynth
-spec set_notification_pid() -> ok.
set_notification_pid() -> erlang:nif_error(nif_not_loaded).

%% @doc Unregister from OSC reply notifications.
-spec clear_notification_pid() -> ok.
clear_notification_pid() -> erlang:nif_error(nif_not_loaded).

