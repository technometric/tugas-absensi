const fs = require('fs');
const path = require('path');
const db = require('./db');

const defaultSql = path.join(__dirname, '..', 'data', 'presensi_slb.sql');
const defaultJson = path.join(__dirname, '..', 'data', 'students.json');
const dumpPath = process.argv[2] ? path.resolve(process.argv[2]) : defaultSql;

function parseSqlValue(token) {
  if (token === 'NULL') return null;
  if (token.startsWith("'") && token.endsWith("'")) {
    const inner = token.slice(1, -1);
    return inner.replace(/\\'/g, "'").replace(/\\\\/g, '\\');
  }
  return token;
}

function splitSqlTuple(text) {
  const values = [];
  let current = '';
  let inString = false;
  let escape = false;

  for (let i = 0; i < text.length; i += 1) {
    const ch = text[i];

    if (escape) {
      current += ch;
      escape = false;
      continue;
    }

    if (ch === '\\') {
      escape = true;
      continue;
    }

    if (ch === "'") {
      inString = !inString;
      current += ch;
      continue;
    }

    if (ch === ',' && !inString) {
      values.push(current.trim());
      current = '';
      continue;
    }

    current += ch;
  }

  if (current.trim().length > 0) {
    values.push(current.trim());
  }

  return values;
}

function parseInserts(sql, tableName) {
  const results = [];
  const insertRegex = new RegExp(
    "INSERT INTO \\\`" + tableName + "\\\` \\\(([^)]+)\\\) VALUES\\s*(.*?);",
    'gs'
  );

  let match;
  while ((match = insertRegex.exec(sql)) !== null) {
    const columns = match[1]
      .split(/`,\s*`/)
      .map((col) => col.replace(/`/g, '').trim());
    const valuesText = match[2];
    const rowRegex = /\(([^)]*)\)/gs;
    let rowMatch;
    while ((rowMatch = rowRegex.exec(valuesText)) !== null) {
      const values = splitSqlTuple(rowMatch[1]);
      const row = {};
      columns.forEach((col, index) => {
        row[col] = parseSqlValue(values[index] || 'NULL');
      });
      results.push(row);
    }
  }

  return results;
}
function loadSqlDump(filePath) {
  if (!fs.existsSync(filePath)) {
    console.warn(`SQL dump tidak ditemukan: ${filePath}`);
    return { students: [], attendances: [] };
  }

  const sql = fs.readFileSync(filePath, 'utf8');
  const students = parseInserts(sql, 'students');
  const attendances = parseInserts(sql, 'attendances');

  return { students, attendances };
}

function loadJsonStudents(filePath) {
  if (!fs.existsSync(filePath)) return [];
  try {
    const raw = fs.readFileSync(filePath, 'utf8');
    const data = JSON.parse(raw);
    return Array.isArray(data) ? data : [];
  } catch (error) {
    console.warn(`Gagal membaca JSON siswa: ${error.message}`);
    return [];
  }
}

function importStudents(rows) {
  const insert = db.connection.prepare(`
    INSERT OR REPLACE INTO students
      (id, name, nisn, class_name, fingerprint_device_id, foto_url, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
  `);

  return db.connection.transaction((items) => {
    let count = 0;
    for (const row of items) {
      insert.run(
        row.id || null,
        row.name || '',
        row.nisn || '',
        row.class_name || '',
        row.fingerprint_device_id || '',
        row.foto_url || '',
        row.created_at || '',
        row.updated_at || ''
      );
      count += 1;
    }
    return count;
  })(rows);
}

function importAttendances(rows) {
  const insert = db.connection.prepare(`
    INSERT OR REPLACE INTO attendances
      (id, student_id, status, tapped_at, attendance_date, note, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
  `);

  return db.connection.transaction((items) => {
    let count = 0;
    for (const row of items) {
      insert.run(
        row.id || null,
        row.student_id || null,
        row.status || 'absent',
        row.tapped_at || null,
        row.attendance_date || '',
        row.note || '',
        row.created_at || '',
        row.updated_at || ''
      );
      count += 1;
    }
    return count;
  })(rows);
}

function main() {
  console.log('Membuat schema database...');
  db.initSchema();

  const { students, attendances } = loadSqlDump(dumpPath);
  console.log(`Menemukan ${students.length} baris siswa dan ${attendances.length} baris absensi di SQL dump.`);

  if (students.length > 0 || attendances.length > 0) {
    console.log('Mengosongkan tabel lokal...');
    db.connection.prepare('DELETE FROM attendances').run();
    db.connection.prepare('DELETE FROM students').run();

    const studentCount = importStudents(students);
    const attendanceCount = importAttendances(attendances);
    console.log(`Impor selesai: ${studentCount} siswa, ${attendanceCount} absensi.`);
  }

  const jsonStudents = loadJsonStudents(defaultJson);
  if (jsonStudents.length > 0) {
    console.log(`Menambahkan ${jsonStudents.length} siswa dari ${defaultJson}`);
    importStudents(jsonStudents.map((student) => ({
      id: student.id || null,
      name: student.name || '',
      nisn: student.nisn || '',
      class_name: student.class_name || '',
      fingerprint_device_id: student.fingerprint_device_id || '',
      foto_url: student.foto_url || '',
      created_at: student.created_at || '',
      updated_at: student.updated_at || '',
    })));
  }

  console.log('Database SQLite siap di', db.path);
}

main();
