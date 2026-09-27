// Phone-side companion JS. The only job here is the Settings page (Clay): everything else this
// watchapp shows comes straight from the firmware's own AppMessage injection (see
// PebbleOS/src/bluetooth-fw/nimble/minimed_sake_sender.c), not from a phone-side companion app.
//
// Clay is vendored as a plain file (clay.js, from pebble-clay's dist.zip) rather than required
// from node_modules: this project's build has no bundler step for phone-side JS, so an npm
// dependency would not actually get packaged. See https://github.com/pebble/clay for the source.
var Clay = require('./clay');
var clayConfig = require('./config');
// eslint-disable-next-line no-unused-vars
var clay = new Clay(clayConfig);
