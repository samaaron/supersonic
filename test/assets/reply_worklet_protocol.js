// Shared message-type constants for the reply_oscchannel_test_worklet
// postMessage protocol. Imported by the worklet asset and re-exported from
// test/fixtures.mjs so spec files can reference the same names.

export const REPLY_WORKLET_MSG = Object.freeze({
  INIT_CHANNEL: 'initChannel',
  READY: 'ready',
  START_COLLECTING: 'startCollecting',
  COLLECTING_STARTED: 'collectingStarted',
  GET_RESULTS: 'getResults',
  RESULTS: 'results',
});
