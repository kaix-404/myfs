const fs = require('fs');
const path = require('path');

const targetFile = path.join('mnt', 'from_node.txt');

// Write a file
fs.writeFileSync(targetFile, 'Hello from Node.js runtime!');
console.log('[Node.js] File created.');

// Read it back
const data = fs.readFileSync(targetFile, 'utf8');
console.log(`[Node.js] Read content: ${data}`);

// Check disk stats
const stats = fs.readFileSync(path.join('mnt', '.diskstat'), 'utf8');
console.log('\n--- Disk Stats via Node.js ---\n' + stats);
