#!/usr/bin/env ruby
# frozen_string_literal: true

require 'webrick'
require 'optparse'

# Default port
PORT = 8002

# Parse command line options
options = {}
OptionParser.new do |opts|
  opts.banner = "Usage: server.rb [options]"
  opts.on("-p", "--port PORT", Integer, "Port to listen on (default: #{PORT})") do |p|
    options[:port] = p
  end
  opts.on("-h", "--help", "Show this help message") do
    puts opts
    exit
  end
end.parse!

port = options[:port] || PORT

# Get the directory where this script is located
SCRIPT_DIR = File.expand_path(File.dirname(__FILE__))

# Custom HTTP server with COOP/COEP headers
class COOPCOEPServlet < WEBrick::HTTPServlet::FileHandler
  def set_cross_origin_headers(response)
    # Enable SharedArrayBuffer by setting cross-origin isolation headers
    response['Cross-Origin-Opener-Policy'] = 'same-origin'
    response['Cross-Origin-Embedder-Policy'] = 'require-corp'

    # Allow resources to be loaded cross-origin
    response['Cross-Origin-Resource-Policy'] = 'cross-origin'

    # Standard security headers
    response['X-Content-Type-Options'] = 'nosniff'
  end

  def do_GET(request, response)
    super
    set_cross_origin_headers(response)
  end

  def do_HEAD(request, response)
    super
    set_cross_origin_headers(response)
  end
end

# Server configuration
server_config = {
  :Port => port,
  :DocumentRoot => SCRIPT_DIR,
  :ServerSoftware => 'scsynth_wasm Example Server',
  :StartCallback => Proc.new {
    puts <<~BANNER

      ╔════════════════════════════════════════════════════════════╗
      ║                 SuperSonic Example Server                  ║
      ╠════════════════════════════════════════════════════════════╣
      ║                                                            ║
      ║  Server running at: http://localhost:#{port}                  ║
      ║                                                            ║
      ║  Open in browser:   http://localhost:#{port}/demo.html        ║
      ║                     http://localhost:#{port}/simple.html      ║
      ║                     http://localhost:#{port}/simple-cdn.html  ║
      ║                                                            ║
      ║  COOP/COEP headers enabled for SharedArrayBuffer support   ║
      ║                                                            ║
      ║  Press Ctrl+C to stop                                      ║
      ║                                                            ║
      ╚════════════════════════════════════════════════════════════╝

    BANNER
  }
}

# Create and configure the server
server = WEBrick::HTTPServer.new(server_config)

# Mount the servlet to handle all requests
server.mount('/', COOPCOEPServlet, SCRIPT_DIR)

# Set up signal handlers for graceful shutdown
['INT', 'TERM'].each do |signal|
  trap(signal) do
    puts "\n\nShutting down server gracefully..."
    server.shutdown
  end
end

# Handle exit to ensure cleanup
at_exit do
  puts "Server stopped cleanly."
end

# Start the server
begin
  server.start
rescue => e
  puts "Error: #{e.message}"
  exit 1
end