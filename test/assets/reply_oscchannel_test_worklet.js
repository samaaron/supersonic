// Test AudioWorklet processor that imports OscChannel and uses
// setReplyHandler() + pollReplies() to receive replies in process().
import { OscChannel } from '/js/lib/osc_channel.js';
import { REPLY_WORKLET_MSG as MSG } from '/test/assets/reply_worklet_protocol.js';

class ReplyOscChannelTestProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.channel = null;
        this.replies = [];
        this.collecting = false;

        // Bound once in the constructor — passed to setReplyHandler so
        // pollReplies() can call it inline per message with zero per-call alloc.
        this._handleReply = (view, offset, length, _sequence) => {
            let end = 0;
            while (end < length && view[offset + end] !== 0) end++;
            let addr = '';
            for (let j = 0; j < end; j++) addr += String.fromCharCode(view[offset + j]);
            this.replies.push(addr);
        };

        this.port.onmessage = (e) => {
            const { data } = e;

            if (data.type === MSG.INIT_CHANNEL) {
                this.channel = OscChannel.fromTransferable(data.channelData);
                this.channel.setReplyHandler(this._handleReply);
                this.port.postMessage({ type: MSG.READY });
            }

            if (data.type === MSG.START_COLLECTING) {
                this.replies = [];
                this.collecting = true;
                this.port.postMessage({ type: MSG.COLLECTING_STARTED });
            }

            if (data.type === MSG.GET_RESULTS) {
                this.collecting = false;
                if (this.channel) {
                    this.channel.clearReplyHandler();
                }
                this.port.postMessage({ type: MSG.RESULTS, replies: this.replies });
            }
        };
    }

    process() {
        if (this.collecting && this.channel) {
            this.channel.pollReplies();
        }
        return true;
    }
}

registerProcessor('reply-oscchannel-test-processor', ReplyOscChannelTestProcessor);
