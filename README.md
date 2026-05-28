# Course Registration System (GTK + Pthreads)

A multithreaded **Course Registration System Simulator** built in **C** using:

* **GTK+3** for the modern graphical user interface
* **POSIX Threads (pthreads)** for concurrency
* **Cairo** for custom analytics visualization

This project simulates concurrent student registrations with **priority scheduling**, **seat management**, **thread synchronization**, and a modern real-time dashboard.

---

# Features

## Core Functionality

* Concurrent student registration simulation
* Priority-based scheduling
* Seat allocation with mutex synchronization
* Deadlock-free thread execution
* Registration logging system
* Real-time analytics dashboard

---

# GUI Features

## Premium Dashboard

* Real-time statistics
* Success / failure tracking
* Enrollment analytics
* Occupancy visualization charts
* Animated modern dark theme UI

## Course Registry

* View all available courses
* Seat occupancy tracking
* Progress bars for course fill percentage
* Sortable table columns

## Registration Logs

* Live registration activity
* Search and filtering
* Priority highlighting
* Status badges

---

# Technologies Used

| Technology    | Purpose                |
| ------------- | ---------------------- |
| C             | Core application logic |
| GTK+3         | GUI framework          |
| Cairo         | Custom graphics/charts |
| POSIX Threads | Concurrency            |
| Mutex Locks   | Thread synchronization |

---

# Project Structure

```bash
.
├── main.c
├── registration_log.txt
└── README.md
```

---

# Thread Synchronization

The system uses:

* `pthread_mutex_t`
* Thread-safe queue operations
* Per-course locking mechanism

Each course has its own mutex:

```c
pthread_mutex_t seatMutex;
```

This prevents race conditions during seat allocation.

---

# Priority Scheduling

Students are categorized into:

* HIGH priority
* LOW priority

High-priority students are processed first using a custom priority queue implementation.

```c
#define HIGH_PRIORITY 1
#define LOW_PRIORITY  0
```

Low-priority threads also include artificial delays:

```c
#define PRIORITY_DELAY_US 60000
```

---

# Simulation Modes

## 1. Mandatory Scenario

Small controlled simulation:

* 10 students
* 3 high-priority students
* Limited seats

Useful for demonstrating synchronization behavior.

---

## 2. Stress Test Scenario

Large-scale simulation:

* 100 students
* 30 high-priority students
* Multiple courses

Used for concurrency testing and performance observation.

---

# Build Requirements

## Linux Packages

Install GTK+3 development libraries:

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install build-essential libgtk-3-dev
```

### Arch Linux

```bash
sudo pacman -S gtk3 base-devel
```

### Fedora

```bash
sudo dnf install gtk3-devel gcc
```

---

# Compilation

Compile using:

```bash
gcc main.c -o registration_system \
    `pkg-config --cflags --libs gtk+-3.0` \
    -lpthread -lm
```

---

# Running the Application

```bash
./registration_system
```

---

# Logging

The application automatically creates:

```bash
registration_log.txt
```

Example log entry:

```text
[12:15:30] Student   4 | Priority: HIGH | Course: CS101 | SUCCESS
```

---

# Dashboard Metrics

The dashboard displays:

* Successful registrations
* Failed registrations
* Success rate
* Total enrolled students
* Total available seats

---

# Visualization

The application includes a custom Cairo-powered chart system showing:

* Course occupancy
* Enrollment percentages
* Real-time analytics

---

# Concurrency Concepts Demonstrated

This project demonstrates:

* Multithreading
* Mutex synchronization
* Shared resource protection
* Race condition prevention
* Producer-consumer style queue management
* Priority scheduling
* Thread-safe logging

---

# Future Improvements

Potential enhancements:

* Database integration
* Dynamic course creation
* User authentication
* Export analytics
* Student profiles
* Networked client-server version
* REST API backend

---

# Screenshots

Add screenshots here:

```markdown
![Dashboard](screenshots/dashboard.png)
![Courses](screenshots/courses.png)
![Logs](screenshots/logs.png)
```

---

# Author

Developed as a concurrent systems / operating systems simulation project using C, GTK, and pthreads.

---

# License

This project is open-source and available under the MIT License.
