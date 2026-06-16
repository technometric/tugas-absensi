const db = require('./db');

console.log('Inisialisasi database SQLite...');
console.log('Database path:', db.path);

try {
  db.initSchema();
  console.log('Schema database siap.');
} catch (error) {
  console.error('Gagal inisialisasi database:', error.message);
  process.exit(1);
}
