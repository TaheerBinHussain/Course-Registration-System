```markdown
# Concurrent Course Registration System

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C-blue.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![GTK3](https://img.shields.io/badge/GTK-3.0-green.svg)](https://www.gtk.org/)
[![Pthreads](https://img.shields.io/badge/threads-POSIX-orange.svg)](https://pubs.opengroup.org/onlinepubs/007908799/xsh/pthread.h.html)

A **high‑performance, deadlock‑free** university course registration simulator built in **C** with **POSIX threads** and a modern **GTK+ 3 graphical interface**.  
It demonstrates concurrent seat allocation, priority‑based scheduling, real‑time logging, and a premium dark‑theme dashboard.

![Dashboard Preview](screenshots/dashboard.png)

---

## 🚀 Features

### Concurrency & Synchronization
- **Thread‑safe seat allocation** using mutexes (no lost updates, no negative seats).
- **Priority queue** for student threads: final‑year (high priority) get a head start.
- **Low‑priority delay** (`nanosleep`) to reduce contention.
- **Deadlock‑free design** – only one mutex locked at a time.

### Simulation Scenarios
- **Mandatory scenario** – 3 courses (CS101‑CS103, total 6 seats) with 10 students (3 high‑priority, 7 low‑priority).
- **Stress test** – 8 courses (total 41 seats) with 100 students (30 high‑priority, 70 low‑priority).

### Modern GTK+ 3 GUI
- **Dark glass‑morphism theme** – gradients, blur effects, neon cyan accents.
- **Dashboard with live statistics** – success/fail counters, occupancy rate, enrolled vs total seats.
- **Cairo‑rendered bar chart** – real‑time seat occupancy per course, with gradient bars and labels.
- **Sortable, searchable log table** – filter registration events, color‑coded results (green/yellow/red).
- **Sidebar navigation** – Dashboard / Courses / Logs with smooth slide transitions.
- **Loading overlay & spinner** – visual feedback during simulation.
- **Header clock** and status dot.

### Logging & Persistence
- **Registration log** written to `registration_log.txt` (CSV‑like format) for audit.
- **In‑memory log table** with real‑time updates.
- **Clear Log** button to reset the log view.

### Performance & Correctness
- Up to **200 student threads** and **10 courses**.
- **Automatic correctness verification** after each run (checks negative seats, mismatch between enrolled and consumed seats).
- **No race conditions** – mutexes protect all shared data.

---

## 📋 Requirements

- **Linux** (or any system with GTK+ 3 and POSIX threads)
- **GCC** (with `-pthread` support)
- **GTK+ 3 development libraries** (`libgtk-3-dev`)
- **Cairo** (usually included with GTK)

#### Install dependencies on Ubuntu/Debian:
```bash
sudo apt update
sudo apt install build-essential libgtk-3-dev
```

#### On Fedora:
```bash
sudo dnf install gcc gtk3-devel
```

---

## 🛠️ Build & Run

1. **Clone the repository**
   ```bash
   git clone https://github.com/yourusername/concurrent-registration.git
   cd concurrent-registration
   ```

2. **Compile the program**
   ```bash
   gcc os_project_gui.c -o register `pkg-config --cflags --libs gtk+-3.0` -lpthread -lm
   ```

3. **Run the application**
   ```bash
   ./register
   ```

> ⚠️ If you see warnings about `gtk_tree_view_set_rules_hint`, they are harmless. You can ignore them or add `-Wno-deprecated-declarations` to the compiler command.

---

## 🖥️ Usage

Once the GUI launches:

- **Mandatory Scenario** – click the *Mandatory* button in the sidebar.  
  Launches 10 students (3 high‑priority) competing for 6 seats.
- **Stress Test** – click *Stress (100/30)*.  
  Launches 100 students (30 high‑priority) competing for 41 seats across 8 courses.
- **View results** – switch between *Dashboard*, *Courses*, and *Logs* tabs.
- **Search logs** – type into the filter box to find specific student, course, or result entries.
- **Clear logs** – click the *Clear Log* button to empty the log table (the file `registration_log.txt` remains unchanged).

During simulation, the control buttons are disabled and a loading overlay appears.  
The final summary (success/fail counts and verification status) is shown on the Dashboard.

---

## 📁 Project Structure

```
concurrent-registration/
├── os_project_gui.c          # Main source code (GUI + backend)
├── registration_log.txt      # Auto‑generated audit log (created at runtime)
├── screenshots/              # (optional) Screenshots for README
│   ├── dashboard.png
│   ├── courses.png
│   └── logs.png
├── LICENSE                   # MIT License
└── README.md                 # This file
```

---

## 🧠 How It Works (Brief)

1. **Course initialisation** – Each course has a mutex‑protected `availableSeats` and an `enrolledIds` array.
2. **Priority queue** – High‑priority requests are enqueued at the head; low‑priority at the tail.
3. **Thread creation** – Students are spawned in priority order. Low‑priority threads call `nanosleep(60 ms)` before their first registration attempt.
4. **Registration attempt** – `tryRegister()` locks the course mutex, checks for duplicates and availability, then (if successful) decrements seats and records the student ID.
5. **Logging** – Every attempt is written to both the file and the in‑memory log store (thread‑safe via `g_idle_add`).
6. **Dashboard updates** – After all threads finish, the course table, stats cards, and bar chart are refreshed.
7. **Verification** – The program checks that `availableSeats` never became negative and that `enrolledCount` matches consumed seats.

---



## 🤝 Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository.
2. Create a feature branch (`git checkout -b feature/amazing`).
3. Commit your changes (`git commit -m 'Add something amazing'`).
4. Push to the branch (`git push origin feature/amazing`).
5. Open a Pull Request.

Please ensure your code adheres to the existing style (no emojis in code, clear function names, no new concurrency bugs).

---

## 🙏 Acknowledgements

- GTK+ team for the amazing widget toolkit.
- POSIX threads (`pthreads`) for portable concurrency.
- Cairo graphics library for smooth chart rendering.

---

## ❓ FAQ

**Q: Why do I see “No Seats” for almost all low‑priority students in the mandatory scenario?**  
A: The mandatory scenario has only 6 seats for 10 students. High‑priority students (first 3) are given a 60 ms head start, so they usually take all seats. This illustrates priority scheduling.

**Q: Can I add my own courses?**  
A: Yes – modify the `setup_stress_courses()` function in the source code. Follow the existing structure (id, name, seats) and ensure you do not exceed `MAX_COURSES`.

**Q: Does the program work on Windows?**  
A: Not directly – GTK+ 3 can be installed on Windows, but POSIX threads (`pthread.h`) require additional emulation (e.g., Cygwin or WSL). For a native Windows build, consider using the Windows Subsystem for Linux (WSL) or rewrite the threading part with Win32 API.

**Q: How do I change the priority delay?**  
A: Edit `PRIORITY_DELAY_US` at the top of `os_project_gui.c`. The value is in microseconds (default 60 000 = 60 ms).
