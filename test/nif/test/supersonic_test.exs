defmodule SupersonicTest do
  use ExUnit.Case

  # When SUPERSONIC_HEADLESS=1 is set (e.g. Windows CI with no audio device),
  # tests boot in headless mode. The HeadlessDriver inside the engine handles
  # audio processing automatically — no manual ticking needed.
  @headless System.get_env("SUPERSONIC_HEADLESS") == "1"

  # Tests share a single global NIF engine — ensure clean state between tests
  setup do
    :supersonic.stop()
    on_exit(fn -> :supersonic.stop() end)
    :ok
  end

  # ── Helpers ──────────────────────────────────────────────────────────────

  defp start_config(overrides \\ %{}) do
    if @headless do
      Map.merge(%{headless: true}, overrides)
    else
      overrides
    end
  end

  # Build a minimal OSC message from an address string.
  # OSC format: null-terminated string padded to 4-byte boundary + type tag ","
  defp osc_message(address) do
    osc_string(address) <> osc_string(",")
  end

  # Build an OSC message with a single int32 argument.
  defp osc_message(address, int_arg) when is_integer(int_arg) do
    osc_string(address) <> osc_string(",i") <> <<int_arg::signed-big-32>>
  end

  # Build an OSC message with a single float32 argument.
  defp osc_message(address, float_arg) when is_float(float_arg) do
    osc_string(address) <> osc_string(",f") <> <<float_arg::float-big-32>>
  end

  defp osc_string(str) do
    bytes = str <> <<0>>
    pad_len = case rem(byte_size(bytes), 4) do
      0 -> 0
      n -> 4 - n
    end
    bytes <> <<0::size(pad_len * 8)>>
  end

  defp wait_for_reply(timeout \\ 2000) do
    receive do
      {:osc_reply, bin} when is_binary(bin) -> {:ok, bin}
    after
      timeout -> :timeout
    end
  end

  # Wait for an OSC reply whose address starts with `prefix`, ignoring any other
  # replies / debug messages that arrive first.
  defp wait_for_reply_matching(prefix, timeout \\ 2000) do
    deadline = System.monotonic_time(:millisecond) + timeout
    do_wait_matching(prefix, deadline)
  end

  defp do_wait_matching(prefix, deadline) do
    remaining = deadline - System.monotonic_time(:millisecond)

    if remaining <= 0 do
      :timeout
    else
      receive do
        {:osc_reply, bin} when is_binary(bin) ->
          if String.starts_with?(bin, prefix),
            do: {:ok, bin},
            else: do_wait_matching(prefix, deadline)

        {:debug, _} ->
          do_wait_matching(prefix, deadline)
      after
        remaining -> :timeout
      end
    end
  end

  # Extract the trailing float64 arg from a single-double OSC reply. (getBpm()
  # returns a double, which oscpack encodes as an 8-byte OSC ',d' arg.)
  defp trailing_double(bin) do
    <<_::binary-size(byte_size(bin) - 8), d::float-big-64>> = bin
    d
  end

  # ── NIF loading ──────────────────────────────────────────────────────────

  test "NIF is loaded" do
    assert :supersonic.is_nif_loaded() == true
  end

  # ── Lifecycle ────────────────────────────────────────────────────────────

  test "start and stop" do
    assert :ok = :supersonic.start(start_config())
    assert :ok = :supersonic.stop()
  end

  test "double start returns error" do
    assert :ok = :supersonic.start(start_config())
    assert {:error, :already_running} = :supersonic.start(start_config())
  end

  test "stop when not running is ok" do
    assert :ok = :supersonic.stop()
  end

  # ── OSC send ─────────────────────────────────────────────────────────────

  test "send_osc when not running returns error" do
    assert {:error, :not_running} = :supersonic.send_osc(osc_message("/status"))
  end

  test "send_osc with valid message returns ok" do
    :ok = :supersonic.start(start_config())
    assert :ok = :supersonic.send_osc(osc_message("/status"))
  end

  test "send_osc with non-binary returns badarg" do
    :ok = :supersonic.start(start_config())
    assert_raise ArgumentError, fn -> :supersonic.send_osc(:not_a_binary) end
  end

  # ── Notifications ────────────────────────────────────────────────────────

  test "set and clear notification pid" do
    assert :ok = :supersonic.set_notification_pid()
    assert :ok = :supersonic.clear_notification_pid()
  end

  test "multiple registered processes each receive replies" do
    :ok = :supersonic.start(start_config())
    test_pid = self()

    # A second process registers itself and relays the first reply it sees.
    relay = spawn(fn ->
      :supersonic.set_notification_pid()
      send(test_pid, :relay_registered)

      receive do
        {:osc_reply, bin} -> send(test_pid, {:relay, bin})
      after
        2000 -> send(test_pid, :relay_timeout)
      end
    end)

    assert_receive :relay_registered, 2000

    # This process registers too, then triggers a reply.
    :ok = :supersonic.set_notification_pid()
    :ok = :supersonic.send_osc(osc_message("/version"))

    # Both audiences receive it — the registry fans out, it isn't single-pid.
    assert {:ok, _} = wait_for_reply_matching("/version.reply")
    assert_receive {:relay, bin} when is_binary(bin), 2000

    Process.exit(relay, :kill)
  end

  # ── Link notify (parity with the UDP transport) ──────────────────────────
  #
  # /clock/notify/subscribe immediately pushes a tempo + peers snapshot via the
  # networkOnly SEND_TO_CALLER route. CallbackTransport drops networkOnly (an
  # in-process observer reads engine state directly); the NIF transport, being a
  # real out-of-process peer, delivers it — proving the Link path now works from
  # BEAM exactly as it does over UDP.

  test "subscribing to Link notify pushes an immediate snapshot to the registered pid" do
    :ok = :supersonic.start(start_config())
    :ok = :supersonic.set_notification_pid()

    :ok = :supersonic.send_osc(osc_message("/clock/notify/subscribe"))

    assert {:ok, _} = wait_for_reply_matching("/clock/notify/tempo")
    assert {:ok, _} = wait_for_reply_matching("/clock/notify/peers")
  end

  test "receive /version.reply" do
    :ok = :supersonic.start(start_config())
    :ok = :supersonic.set_notification_pid()
    :ok = :supersonic.send_osc(osc_message("/version"))

    assert {:ok, reply} = wait_for_reply()
    assert is_binary(reply)
    assert String.starts_with?(reply, "/version.reply")
  end

  test "receive /g_queryTree.reply" do
    :ok = :supersonic.start(start_config())
    :ok = :supersonic.set_notification_pid()
    :ok = :supersonic.send_osc(osc_message("/g_queryTree", 0))

    assert {:ok, reply} = wait_for_reply()
    assert is_binary(reply)
    assert String.starts_with?(reply, "/g_queryTree.reply")
  end

  # ── Link / clock queries (travel the NRT plane, not the RT OUT ring) ──────
  #
  # A /clock query is forwarded to the NRT command ring, processed on the gateway
  # thread (EngineControl/SuperClock), and its reply is framed into the NRT-out
  # ring. The gateway merges that with the RT OUT ring and the reply reaches the
  # registered pid as an {:osc_reply, binary} message — exactly the same sink as
  # scsynth's own replies, proving the unified egress works for the NRT plane.

  test "clock tempo query round-trips via the NRT egress ring" do
    :ok = :supersonic.start(start_config())
    :ok = :supersonic.set_notification_pid()

    :ok = :supersonic.send_osc(osc_message("/clock/tempo/set", 142.5))
    :ok = :supersonic.send_osc(osc_message("/clock/tempo/get"))

    # /clock/tempo.reply travels the NRT command ring → gateway → NRT-out ring →
    # the registered pid, and the tempo value round-trips (read from the shared
    # SuperClock state mirror, same as the web build).
    assert {:ok, reply} = wait_for_reply_matching("/clock/tempo.reply")
    assert_in_delta trailing_double(reply), 142.5, 0.5
  end

  test "clock visibility query replies to the registered pid via the NRT ring" do
    :ok = :supersonic.start(start_config())
    :ok = :supersonic.set_notification_pid()

    :ok = :supersonic.send_osc(osc_message("/clock/visibility/get"))

    assert {:ok, reply} = wait_for_reply_matching("/clock/visibility.reply")
    assert is_binary(reply)
  end

  test "RT (scsynth) and NRT (clock) replies both reach the same pid" do
    :ok = :supersonic.start(start_config())
    :ok = :supersonic.set_notification_pid()

    # /version → scsynth reply on the RT OUT ring.
    :ok = :supersonic.send_osc(osc_message("/version"))
    assert {:ok, _} = wait_for_reply_matching("/version.reply")

    # /clock/tempo/get → reply on the NRT-out ring. Same registered pid.
    :ok = :supersonic.send_osc(osc_message("/clock/tempo/get"))
    assert {:ok, _} = wait_for_reply_matching("/clock/tempo.reply")
  end

  # ── Config options ───────────────────────────────────────────────────────

  test "start with custom config" do
    config = start_config(%{
      sample_rate: 44100,
      num_output_channels: 2,
      num_input_channels: 0,
      max_nodes: 512,
      num_buffers: 256
    })
    assert :ok = :supersonic.start(config)
  end
end
