const Database = require('better-sqlite3');
const path = require('path');

const pathToDb = path.join(__dirname, 'db.sqlite');
const db = new Database(pathToDb);

function initSchema() {
  db.exec(`
    PRAGMA journal_mode = WAL;
    PRAGMA foreign_keys = ON;

    CREATE TABLE IF NOT EXISTS students (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT NOT NULL,
      nisn TEXT,
      class_name TEXT NOT NULL,
      fingerprint_device_id TEXT NOT NULL UNIQUE,
      foto_url TEXT DEFAULT '',
      created_at TEXT NOT NULL DEFAULT (datetime('now')),
      updated_at TEXT NOT NULL DEFAULT (datetime('now'))
    );

    CREATE TABLE IF NOT EXISTS attendances (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      student_id INTEGER NOT NULL,
      status TEXT NOT NULL CHECK(status IN ('present','sick','permission','absent')),
      tapped_at TEXT NOT NULL,
      attendance_date TEXT NOT NULL,
      note TEXT,
      created_at TEXT NOT NULL DEFAULT (datetime('now')),
      updated_at TEXT NOT NULL DEFAULT (datetime('now')),
      FOREIGN KEY(student_id) REFERENCES students(id) ON DELETE CASCADE
    );
  `);
}

function getStudents() {
  return db.prepare(`
    SELECT id, name, nisn, class_name, fingerprint_device_id, foto_url, created_at, updated_at
    FROM students
    ORDER BY name COLLATE NOCASE ASC
  `).all();
}

function getStudentById(id) {
  return db.prepare(`
    SELECT id, name, nisn, class_name, fingerprint_device_id, foto_url, created_at, updated_at
    FROM students
    WHERE id = ?
  `).get(id);
}

function addStudent(student) {
  const now = new Date().toISOString();
  const info = db.prepare(`
    INSERT INTO students (name, nisn, class_name, fingerprint_device_id, foto_url, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?)
  `).run(student.name, student.nisn || '', student.class_name, student.fingerprint_device_id, student.foto_url || '', now, now);
  return getStudentById(info.lastInsertRowid);
}

function deleteStudent(id) {
  const info = db.prepare(`DELETE FROM students WHERE id = ?`).run(id);
  return info.changes > 0;
}

function getAttendances(date) {
  return db.prepare(`
    SELECT a.id, a.student_id, a.status, a.tapped_at, a.attendance_date, a.note,
           s.name AS name, s.nisn AS nisn, s.class_name, s.foto_url, s.fingerprint_device_id
    FROM attendances a
    JOIN students s ON s.id = a.student_id
    WHERE a.attendance_date = ?
    ORDER BY a.tapped_at DESC
  `).all(date);
}

function addAttendance(attendance) {
  const now = new Date().toISOString();
  const info = db.prepare(`
    INSERT INTO attendances (student_id, status, tapped_at, attendance_date, note, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?)
  `).run(
    attendance.student_id,
    attendance.status,
    attendance.tapped_at,
    attendance.attendance_date,
    attendance.note || '',
    now,
    now
  );
  return db.prepare('SELECT * FROM attendances WHERE id = ?').get(info.lastInsertRowid);
}

function getDashboard(date) {
  const totalStudents = db.prepare('SELECT COUNT(*) AS count FROM students').get().count;
  const stats = db.prepare(`
    SELECT
      SUM(CASE WHEN status = 'present' THEN 1 ELSE 0 END) AS present,
      SUM(CASE WHEN status = 'sick' THEN 1 ELSE 0 END) AS sick,
      SUM(CASE WHEN status = 'permission' THEN 1 ELSE 0 END) AS permission,
      SUM(CASE WHEN status = 'absent' THEN 1 ELSE 0 END) AS absent,
      COUNT(*) AS total
    FROM attendances
    WHERE attendance_date = ?
  `).get(date);

  const latest = db.prepare(`
    SELECT a.id, a.status, a.tapped_at, a.attendance_date,
           s.name, s.nisn, s.class_name, s.foto_url, s.fingerprint_device_id
    FROM attendances a
    JOIN students s ON s.id = a.student_id
    WHERE a.attendance_date = ?
    ORDER BY a.tapped_at DESC
    LIMIT 1
  `).get(date);

  const presentCount = stats.present || 0;
  const sisa = Math.max(totalStudents - presentCount, 0);
  const persen = totalStudents > 0 ? Math.round((presentCount / totalStudents) * 100) : 0;

  return {
    total_students: totalStudents,
    total_present: presentCount,
    total_sick: stats.sick || 0,
    total_permission: stats.permission || 0,
    total_absent: stats.absent || 0,
    total_attendance: stats.total || 0,
    total_remaining: sisa,
    percent_present: persen,
    latest_attendance: latest || null,
  };
}

initSchema();

module.exports = {
  path: pathToDb,
  version: db.pragma('user_version', { simple: true }),
  connection: db,
  initSchema,
  getStudents,
  getStudentById,
  addStudent,
  deleteStudent,
  getAttendances,
  addAttendance,
  getDashboard,
};
