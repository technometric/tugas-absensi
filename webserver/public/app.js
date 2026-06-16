const STORAGE_KEY = 'esp32_host';
let esp32Host = localStorage.getItem(STORAGE_KEY) || '';

const hostInput = document.getElementById('esp32-host');
const saveHostButton = document.getElementById('save-host');
const esp32StatusEl = document.getElementById('esp32-status');
const totalSiswaEl = document.getElementById('total-siswa');
const totalHadirEl = document.getElementById('total-hadir');
const totalSisaEl = document.getElementById('total-sisa');
const persenHadirEl = document.getElementById('persen-hadir');
const totalTolakEl = document.getElementById('total-tolak');
const latestNameEl = document.getElementById('latest-name');
const latestIdEl = document.getElementById('latest-id');
const latestStatusEl = document.getElementById('latest-status');
const latestTimeEl = document.getElementById('latest-time');
const latestClassEl = document.getElementById('latest-class');
const latestNisnEl = document.getElementById('latest-nisn');
const latestPhotoImg = document.getElementById('latest-photo-img');
const historyBodyEl = document.getElementById('history-body');
const enrollMessage = document.getElementById('enroll-message');
const enrollButton = document.getElementById('enroll-start');
const enrollCancel = document.getElementById('enroll-cancel');
const enrollName = document.getElementById('enroll-name');
const enrollNisn = document.getElementById('enroll-nisn');
const enrollClass = document.getElementById('enroll-class');
const enrollId = document.getElementById('enroll-id');
const studentsList = document.getElementById('students-list');
const attendanceStatus = document.getElementById('attendance-status');
const attendanceFingerId = document.getElementById('attendance-finger-id');
const attendanceName = document.getElementById('attendance-name');
const attendanceClass = document.getElementById('attendance-class');
const attendanceTime = document.getElementById('attendance-time');
const liveStream = document.getElementById('live-stream');
const attendancePopup = document.getElementById('attendance-popup');
const popupPhoto = document.getElementById('popup-photo');
const popupName = document.getElementById('popup-name');
const popupClass = document.getElementById('popup-class');
const popupNisn = document.getElementById('popup-nisn');
const popupId = document.getElementById('popup-id');
const popupTime = document.getElementById('popup-time');
const hostHelp = document.getElementById('host-help');
const tabs = document.querySelectorAll('.tab');

const sections = document.querySelectorAll('.section');
let enrollmentInProgress = false;
let enrollmentCancelRequested = false;
let esp32Available = false;
let attendancePopupTimeout = null;
let lastAttendanceKey = '';
let liveStreamRunning = false;

function setEsp32Host(value) {
  esp32Host = value.trim();
  localStorage.setItem(STORAGE_KEY, esp32Host);
  hostInput.value = esp32Host;
  esp32Available = false;
  updateStatus(esp32Host ? `Menguji koneksi ke ESP32 ${esp32Host}...` : 'ESP32 host dihapus.');
  updateControls();
  if (esp32Host) {
    validateEsp32Host();
  } else {
    refreshAll();
  }
}

async function validateEsp32Host() {
  try {
    const status = await fetchEsp32Json('/status');
    esp32Available = true;
    updateStatus(`ESP32 terhubung: ${status.device || esp32Host}`);
    updateControls();
    refreshAll();
  } catch (error) {
    esp32Available = false;
    updateStatus(`Gagal terhubung ke ESP32: ${error.message}`);
    updateControls();
    refreshAll();
  }
}

function getEsp32BaseUrl() {
  return `http://${esp32Host}`;
}

function getEsp32ProxyUrl(path) {
  const separator = path.includes('?') ? '&' : '?';
  return `/esp32${path}${separator}host=${encodeURIComponent(esp32Host)}`;
}

function updateControls() {
  const enabled = !!esp32Host && esp32Available;
  enrollButton.disabled = !enabled || enrollmentInProgress;
  enrollCancel.disabled = !enrollmentInProgress;
  enrollName.disabled = !enabled;
  enrollNisn.disabled = !enabled;
  enrollClass.disabled = !enabled;
  enrollId.disabled = !enabled;
  hostHelp.textContent = esp32Host
    ? esp32Available
      ? 'ESP32 sudah terkoneksi. Silakan mulai enrollment.'
      : 'IP ESP32 disimpan tetapi tidak bisa dihubungi. Periksa koneksi dan coba lagi.'
    : 'Isi IP ESP32 lalu klik Simpan. Enrollment hanya bekerja jika ESP32 terkoneksi.';
}

function setEnrollmentBusy(busy) {
  enrollmentInProgress = busy;
  if (!busy) {
    enrollmentCancelRequested = false;
  }
  updateControls();
  if (busy) {
    enrollButton.textContent = 'Menunggu ESP32...';
  } else {
    enrollButton.textContent = 'Mulai Enrollment';
  }
}

function setActiveSection(name) {
  sections.forEach((section) => {
    section.classList.toggle('active', section.id === name);
  });
  tabs.forEach((tab) => {
    tab.classList.toggle('active', tab.dataset.section === name);
  });
}

function updateStatus(text) {
  esp32StatusEl.textContent = text || 'Menunggu konfigurasi ESP32...';
}

function updateAttendancePanel(attendance) {
  attendanceFingerId.textContent = attendance.fingerprint_device_id || '-';
  attendanceName.textContent = attendance.name || '-';
  attendanceClass.textContent = attendance.class_name || '-';
  attendanceTime.textContent = attendance.tapped_at || '-';
}

function showAttendancePopup(attendance) {
  popupPhoto.src = buildFotoUrl(attendance.foto_url || '');
  popupName.textContent = attendance.name || '-';
  popupClass.textContent = attendance.class_name || '-';
  popupNisn.textContent = attendance.nisn || '-';
  popupId.textContent = attendance.fingerprint_device_id || '-';
  popupTime.textContent = attendance.tapped_at || '-';
  attendancePopup.classList.remove('hidden');

  if (attendancePopupTimeout) clearTimeout(attendancePopupTimeout);
  attendancePopupTimeout = setTimeout(() => {
    attendancePopup.classList.add('hidden');
  }, 3500);
}

function buildFotoUrl(relativePath) {
  if (!relativePath || !esp32Host) return '';
  return `http://${esp32Host}/foto?path=${encodeURIComponent(relativePath)}`;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function stopLiveStream() {
  if (!liveStreamRunning && !liveStream.src) return;
  liveStreamRunning = false;
  liveStream.removeAttribute('src');
}

function startLiveStream() {
  if (!esp32Host || !esp32Available) return;
  const streamUrl = `http://${esp32Host}/stream`;
  if (liveStream.src !== streamUrl) {
    liveStream.src = streamUrl;
  }
  liveStreamRunning = true;
}

async function fetchJson(path) {
  const response = await fetch(path, { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  return response.json();
}

async function fetchEsp32Json(path, options = {}) {
  if (!esp32Host) throw new Error('ESP32 host belum dikonfigurasi');
  if (options.pauseStream !== false && liveStreamRunning) {
    stopLiveStream();
    await sleep(250);
  }

  const url = getEsp32ProxyUrl(path);
  const response = await fetch(url, { cache: 'no-store' });
  const body = await response.text();
  let data = null;
  if (body) {
    try {
      data = JSON.parse(body);
    } catch (error) {
      if (response.ok) throw new Error('Response ESP32 bukan JSON valid');
    }
  }

  if (!response.ok) {
    const detail = data?.detail || data?.error || body || response.statusText;
    throw new Error(`ESP32 HTTP ${response.status}: ${detail}`);
  }
  return data;
}

async function refreshDashboard() {
  try {
    const data = await fetchJson('/api/dashboard');
    updateStatus(esp32Host ? `ESP32 host: ${esp32Host}` : 'ESP32 belum dikonfigurasi, data lokal ditampilkan.');
    totalSiswaEl.textContent = data.total_students;
    totalHadirEl.textContent = data.total_present;
    totalSisaEl.textContent = data.total_remaining;
    persenHadirEl.textContent = `${data.percent_present}%`;
    totalTolakEl.textContent = data.total_absent + data.total_sick + data.total_permission;

    const latest = data.latest_attendance || {};
    latestNameEl.textContent = latest.name || '-';
    latestIdEl.textContent = latest.fingerprint_device_id || '-';
    latestStatusEl.textContent = latest.status || '-';
    latestTimeEl.textContent = latest.tapped_at || '-';
    latestClassEl.textContent = latest.class_name || '-';
    latestNisnEl.textContent = latest.nisn || '-';

    const photoUrl = buildFotoUrl(latest.foto_url || '');
    if (photoUrl) {
      latestPhotoImg.src = photoUrl;
      latestPhotoImg.style.display = 'block';
    } else {
      latestPhotoImg.style.display = 'none';
    }

    if (Array.isArray(data.history)) {
      renderHistory(data.history);
    } else {
      historyBodyEl.innerHTML = '<tr><td colspan="6" class="empty">Belum ada riwayat absensi.</td></tr>';
    }
  } catch (error) {
    updateStatus(`Gagal ambil data lokal: ${error.message}`);
    historyBodyEl.innerHTML = '<tr><td colspan="6" class="empty">Tidak dapat memuat riwayat.</td></tr>';
  }
}

async function refreshAttendance() {
  if (!esp32Host || !esp32Available) {
    attendanceStatus.textContent = 'ESP32 belum siap untuk absensi.';
    stopLiveStream();
    return;
  }

  attendanceStatus.textContent = 'Menunggu finger...';

  try {
    const data = await fetchEsp32Json('/api');
    const latest = data.latest_absensi || {};
    if (latest.fingerprint_device_id && latest.name) {
      const attendanceKey = `${latest.fingerprint_device_id}_${latest.tapped_at}`;
      if (attendanceKey !== lastAttendanceKey) {
        lastAttendanceKey = attendanceKey;
        attendanceStatus.textContent = `Finger valid: ${latest.name} (${latest.fingerprint_device_id})`;
        updateAttendancePanel(latest);
        showAttendancePopup(latest);
        saveLocalAttendance(latest);
      }
    }
  } catch (error) {
    attendanceStatus.textContent = `Gagal ambil status absensi: ${error.message}`;
  } finally {
    startLiveStream();
  }
}

async function saveLocalAttendance(latest) {
  if (!latest.student_id && !latest.fingerprint_device_id) return;
  const studentId = latest.student_id || await resolveStudentId(latest.fingerprint_device_id);
  if (!studentId) {
    console.warn('Tidak dapat menemukan student_id lokal untuk finger', latest.fingerprint_device_id);
    return;
  }

  const status = latest.status || 'present';
  const tappedAt = latest.tapped_at || new Date().toISOString();
  const attendanceDate = tappedAt.slice(0, 10);

  try {
    await fetch('/api/attendances', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        student_id: studentId,
        status,
        tapped_at: tappedAt,
        attendance_date: attendanceDate,
        note: 'absensi dari ESP32',
      }),
    });
    refreshDashboard();
  } catch (error) {
    console.warn('Gagal simpan attendance lokal:', error);
  }
}

async function resolveStudentId(fingerprintId) {
  try {
    const students = await fetchJson('/api/students');
    const student = students.find((s) => String(s.fingerprint_device_id) === String(fingerprintId));
    return student ? student.id : null;
  } catch (error) {
    return null;
  }
}

function renderHistory(rows) {
  historyBodyEl.innerHTML = '';
  if (!Array.isArray(rows) || rows.length === 0) {
    historyBodyEl.innerHTML = '<tr><td colspan="6" class="empty">Belum ada absensi hari ini.</td></tr>';
    return;
  }

  rows.slice().reverse().forEach((row) => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${row.tapped_at || '-'}</td>
      <td>${row.name || '-'}</td>
      <td>${row.fingerprint_device_id || '-'}</td>
      <td>${row.status_label || '-'}</td>
      <td>${row.class_name || '-'}</td>
      <td>${row.foto_url ? `<a href="${buildFotoUrl(row.foto_url)}" target="_blank">Lihat</a>` : '-'}</td>
    `;
    historyBodyEl.appendChild(tr);
  });
}

async function loadStudents() {
  try {
    const students = await fetchJson('/api/students');
    if (!Array.isArray(students) || students.length === 0) {
      studentsList.innerHTML = '<p class="empty">Belum ada siswa terdaftar.</p>';
      return;
    }

    let html = '<table class="student-table"><thead><tr><th>ID Fingerprint</th><th>Nama</th><th>NISN</th><th>Kelas</th><th>Foto</th></tr></thead><tbody>';
    students.forEach((student) => {
      const foto = buildFotoUrl(student.foto_url);
      html += `<tr><td>${student.fingerprint_device_id}</td><td>${student.name}</td><td>${student.nisn}</td><td>${student.class_name}</td>`;
      html += `<td>${foto ? `<img class="student-photo" src="${foto}" alt="Foto" />` : '—'}</td></tr>`;
    });
    html += '</tbody></table>';
    studentsList.innerHTML = html;
  } catch (error) {
    studentsList.innerHTML = `<p class="empty">Gagal memuat data siswa: ${error.message}</p>`;
  }
}

async function startEnrollment() {
  if (!esp32Host) {
    updateStatus('Masukkan IP ESP32 terlebih dahulu.');
    return;
  }

  const name = enrollName.value.trim();
  const nisn = enrollNisn.value.trim();
  const className = enrollClass.value.trim();
  const id = Number(enrollId.value);
  if (!name || !id || id < 1 || id > 127) {
    enrollMessage.textContent = 'Nama dan ID fingerprint wajib diisi dengan benar.';
    enrollMessage.className = 'message fail';
    return;
  }

  setEnrollmentBusy(true);
  enrollMessage.textContent = 'Mulai enrollment... Silakan tempelkan jari ke sensor.';
  enrollMessage.className = 'message info';

  try {
    const query = new URLSearchParams({
      name,
      nisn,
      class_name: className,
      id: id.toString(),
    });
    const data = await fetchEsp32Json(`/enroll-start?${query}`);
    enrollMessage.textContent = data.message || 'Proses enrollment dimulai.';
    enrollMessage.className = data.ok ? 'message suc' : 'message fail';
    if (data.ok) {
      pollEnrollment();
      try {
        await fetch('/api/students', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            name,
            nisn,
            class_name: className,
            fingerprint_device_id: id.toString(),
            foto_url: '',
          }),
        });
      } catch (ignored) {
        // Local DB sync may fail if student sudah terdaftar.
      }
    } else {
      setEnrollmentBusy(false);
    }
  } catch (error) {
    enrollMessage.textContent = `Gagal memulai enrollment: ${error.message}`;
    enrollMessage.className = 'message fail';
    setEnrollmentBusy(false);
  }
}

async function pollEnrollment() {
  if (enrollmentCancelRequested) {
    enrollMessage.textContent = 'Enrollment dibatalkan.';
    enrollMessage.className = 'message fail';
    setEnrollmentBusy(false);
    return;
  }

  try {
    const data = await fetchEsp32Json('/enroll-poll');
    enrollMessage.textContent = data.message || 'Menunggu scan...';
    enrollMessage.className = data.success ? 'message suc' : 'message info';
    if (!data.done) {
      setTimeout(pollEnrollment, 1200);
      return;
    }
    setEnrollmentBusy(false);
    if (data.success) {
      enrollMessage.className = 'message suc';
      setTimeout(() => {
        setActiveSection('students');
        loadStudents();
      }, 1500);
    } else {
      enrollMessage.className = 'message fail';
    }
  } catch (error) {
    enrollMessage.textContent = `Gagal polling enrollment: ${error.message}`;
    enrollMessage.className = 'message fail';
    setEnrollmentBusy(false);
  }
}

function resetEnrollmentForm() {
  enrollName.value = '';
  enrollNisn.value = '';
  enrollClass.value = '';
  enrollId.value = '';
  enrollMessage.textContent = 'Silakan isi data dan klik Mulai Enrollment.';
  enrollMessage.className = 'message info';
  enrollmentCancelRequested = true;
  setEnrollmentBusy(false);
}

function refreshAll() {
  refreshDashboard();
  loadStudents();
  refreshAttendance();
}

saveHostButton.addEventListener('click', () => setEsp32Host(hostInput.value));

tabs.forEach((tab) => {
  tab.addEventListener('click', () => setActiveSection(tab.dataset.section));
});

enrollButton.addEventListener('click', startEnrollment);
enrollCancel.addEventListener('click', resetEnrollmentForm);

if (esp32Host) {
  hostInput.value = esp32Host;
  updateStatus(`Menguji koneksi ke ESP32 ${esp32Host}...`);
  updateControls();
  validateEsp32Host();
} else {
  updateStatus('Masukkan IP ESP32 dan klik Simpan.');
  updateControls();
}

setInterval(() => {
  refreshDashboard();
}, 5000);
