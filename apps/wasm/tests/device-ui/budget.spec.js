// @ts-check
'use strict';

// The flash budget planning/esp32-device-ui.md sets for the page: the page and
// its script together, as the firmware embeds them, within 16,384 bytes. LF
// line endings too (.gitattributes pins them), so a Windows checkout embeds,
// budgets and maps coverage onto the same bytes CI does.

const fs = require('fs');
const path = require('path');
const { test, expect } = require('@playwright/test');
const { UI_DIR } = require('./stub');

const BUDGET = 16384;

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
