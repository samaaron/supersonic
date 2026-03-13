defmodule SupersonicTest do
  use ExUnit.Case

  # Tests share a single global NIF engine — ensure clean state between tests
  setup do
    :supersonic.stop()
    on_exit(fn -> :supersonic.stop() end)
    :ok
  end

  # ── Helpers ──────────────────────────────────────────────────────────────

  # Build a minimal OSC message from an address string.
  # OSC format: null-terminated string padded to 4-byte boundary + type tag ","
  defp osc_message(address) do
    osc_string(address) <> osc_string(",")
  end

  # Build an OSC message with a single int32 argument.
  defp osc_message(address, int_arg) when is_integer(int_arg) do
    osc_string(address) <> osc_string(",i") <> <<int_arg::signed-big-32>>
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

  # ── NIF loading ──────────────────────────────────────────────────────────

  test "NIF is loaded" do
    assert :supersonic.is_nif_loaded() == true
  end

  # ── Lifecycle ────────────────────────────────────────────────────────────

  test "start and stop" do
    assert :ok = :supersonic.start(%{})
    assert :ok = :supersonic.stop()
  end

  test "double start returns error" do
    assert :ok = :supersonic.start(%{})
    assert {:error, :already_running} = :supersonic.start(%{})
  end

  test "stop when not running is ok" do
    assert :ok = :supersonic.stop()
  end

  # ── OSC send ─────────────────────────────────────────────────────────────

  test "send_osc when not running returns error" do
    assert {:error, :not_running} = :supersonic.send_osc(osc_message("/status"))
  end

  test "send_osc with valid message returns ok" do
    :ok = :supersonic.start(%{})
    assert :ok = :supersonic.send_osc(osc_message("/status"))
  end

  test "send_osc with non-binary returns badarg" do
    :ok = :supersonic.start(%{})
    assert_raise ArgumentError, fn -> :supersonic.send_osc(:not_a_binary) end
  end

  # ── Notifications ────────────────────────────────────────────────────────

  test "set and clear notification pid" do
    assert :ok = :supersonic.set_notification_pid()
    assert :ok = :supersonic.clear_notification_pid()
  end

  test "receive /version.reply" do
    :ok = :supersonic.start(%{})
    :ok = :supersonic.set_notification_pid()
    :ok = :supersonic.send_osc(osc_message("/version"))

    assert {:ok, reply} = wait_for_reply()
    assert is_binary(reply)
    # /version.reply starts with the address pattern
    assert String.starts_with?(reply, "/version.reply")
  end

  test "receive /g_queryTree.reply" do
    :ok = :supersonic.start(%{})
    :ok = :supersonic.set_notification_pid()
    :ok = :supersonic.send_osc(osc_message("/g_queryTree", 0))

    assert {:ok, reply} = wait_for_reply()
    assert is_binary(reply)
    assert String.starts_with?(reply, "/g_queryTree.reply")
  end

  # ── Config options ───────────────────────────────────────────────────────

  test "start with custom config" do
    config = %{
      sample_rate: 44100,
      num_output_channels: 2,
      num_input_channels: 0,
      max_nodes: 512,
      num_buffers: 256
    }
    assert :ok = :supersonic.start(config)
  end
end
