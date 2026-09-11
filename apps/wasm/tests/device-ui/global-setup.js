'use strict';

// Each run's coverage stands alone: c8 reads every file in the directory the
// fixtures write to, so the previous run's go first.

const fs = require('fs');
const path = require('path');

module.exports = () => {
    fs.rmSync(path.resolve(__dirname, '..', 'test-results', 'device-ui-coverage'), {
        recursive: true,
        force: true,
    });
};
