// @ts-check
'use strict';

// The flash budget planning/esp32-device-ui.md sets for the page (decision 18):
// the page and its script together, as the firmware embeds them. LF
// line endings too (.gitattributes pins them), so a Windows checkout embeds,
// budgets and maps coverage onto the same bytes CI does.
//
// 24,576 since Hearth B2, re-derived rather than raised to fit: the page lost
// the playback controls, which a server now owns, and gained the settings that
// are the board's own - a name, a network, a slot width, the wiring.
//
// 28,672 since Hearth B3: the Sendspin player's section - the server, the
// link, the clock, how late the stream plays, underruns, a level per output,
// a pairing code and the three pairing actions. planning/esp32-device-ui.md,
// decision 18, has the image sizes behind it: the budget is a statement about
// keeping the page small enough to read and serve from flash rather than
// about running out of room.

const fs = require('fs');
const path = require('path');
const { test, expect } = require('@playwright/test');
const { UI_DIR } = require('./stub');

const BUDGET = 28672;

test('the page and its script fit the flash budget', () => {
    const files = ['ac3forge_ui.html', 'ac3forge_ui.js'].map((name) => fs.readFileSync(path.join(UI_DIR, name)));
    const total = files.reduce((sum, file) => sum + file.length, 0);
    test.info().annotations.push({
        type: 'flash',
        description: `${files.map((f) => f.length).join(' + ')} = ${total} of ${BUDGET} bytes`,
    });
    for (const file of files) {
        expect(file.includes(13), 'a carriage return in a file the firmware embeds').toBe(false);
    }
    expect(total).toBeLessThanOrEqual(BUDGET);
});
