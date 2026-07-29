const express = require('express');
const cors = require('cors');
const path = require('path');
const db = require('./db');

const app = express();
const port = parseInt(process.env.PORT || '3000', 10);

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

app.get('/favicon.ico', (req, res) => {
  res.status(204).end();
});

function normalizeEsp32Host(host) {
  const cleanHost = String(host || '').trim().replace(/^https?:\/\//, '').replace(/\/.*$/, '');
  if (!/^[a-zA-Z0-9.-]+(?::\d{1,5})?$/.test(cleanHost)) return '';
  return cleanHost;
}

app.get('/esp32/*', async (req, res) => {
  const esp32Host = normalizeEsp32Host(req.query.host);
  if (!esp32Host) {
    return res.status(400).json({ error: 'host ESP32 tidak valid' });
  }

  const esp32Path = req.params[0] || '';
  const query = new URLSearchParams(req.query);
  query.delete('host');
  const targetUrl = `http://${esp32Host}/${esp32Path}${query.toString() ? `?${query}` : ''}`;

  try {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 10000);
    const response = await fetch(targetUrl, {
      headers: { Accept: 'application/json,text/plain,*/*' },
      signal: controller.signal,
    });
    clearTimeout(timeout);

    const body = await response.text();
    res
      .status(response.status)
      .type(response.headers.get('content-type') || 'application/json')
      .send(body);
  } catch (error) {
    res.status(502).json({
      error: 'Gagal menghubungi ESP32',
      detail: error.name === 'AbortError' ? 'Timeout' : error.message,
    });
  }
});

app.get('/api/status', (req, res) => {
  res.json({
    status: 'ok',
    version: db.version,
    database: db.path,
  });
});

app.get('/api/students', (req, res) => {
  res.json(db.getStudents());
});

app.post('/api/students', (req, res) => {
  const { name, nisn, class_name, fingerprint_device_id } = req.body;
  if (!name || !class_name || !fingerprint_device_id) {
    return res.status(400).json({ error: 'name, class_name, and fingerprint_device_id are required' });
  }
  const student = db.addStudent({ name, nisn, class_name, fingerprint_device_id });
  res.status(201).json(student);
});

app.delete('/api/students/:id', (req, res) => {
  const deleted = db.deleteStudent(req.params.id);
  if (!deleted) return res.status(404).json({ error: 'Student not found' });
  res.json({ deleted: true });
});

app.post('/api/attendances', (req, res) => {
  const { student_id, status, tapped_at, note } = req.body;
  if (!student_id || !status || !tapped_at) {
    return res.status(400).json({ error: 'student_id, status, and tapped_at are required' });
  }
  const attendance = db.addAttendance({ student_id, status, tapped_at, note });
  res.status(201).json(attendance);
});

app.get('/api/dashboard', (req, res) => {
  const date = req.query.date || new Date().toISOString().slice(0, 10);
  const dashboardData = db.getDashboard(date);
  const history = db.getAttendances(date).map((row) => ({
    ...row,
    status_label: row.status,
  }));

  res.json({
    ...dashboardData,
    history,
  });
});

app.listen(port, () => {
  console.log(`Web dashboard running: http://localhost:${port}`);
});
