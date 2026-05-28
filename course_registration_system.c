#define _GNU_SOURCE
#include <strings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <gtk/gtk.h>
#include <cairo.h>

/* ==================== BACKEND (UNCHANGED) ==================== */
#define MAX_COURSES         10
#define MAX_STUDENTS        200
#define MAX_COURSES_PER_STU 3
#define MAX_NAME_LEN        32
#define MAX_ENROLLED        200

#define HIGH_PRIORITY       1
#define LOW_PRIORITY        0
#define PRIORITY_DELAY_US   60000

typedef struct {
    int             courseId;
    char            courseName[MAX_NAME_LEN];
    int             totalSeats;
    int             availableSeats;
    int             enrolledIds[MAX_ENROLLED];
    int             enrolledCount;
    pthread_mutex_t seatMutex;
} Course;

typedef struct {
    int studentId;
    int priority;
    int courseIndices[MAX_COURSES_PER_STU];
    int numCoursesToTry;
} StudentRequest;

typedef struct PQNode {
    StudentRequest  *request;
    struct PQNode   *next;
} PQNode;

typedef struct {
    PQNode          *head;
    int              size;
    pthread_mutex_t  pqMutex;
} PriorityQueue;

Course          courses[MAX_COURSES];
int             numCourses = 0;
int             totalSuccess = 0;
int             totalFailed  = 0;
pthread_mutex_t statsMutex = PTHREAD_MUTEX_INITIALIZER;
FILE           *logFp = NULL;
pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;

/* ==================== PREMIUM GUI GLOBALS ==================== */
GtkWidget      *main_window;
GtkWidget      *sidebar;
GtkWidget      *stack;
GtkWidget      *dashboard_page;
GtkWidget      *courses_page;
GtkWidget      *logs_page;

GtkWidget      *stats_success_label;
GtkWidget      *stats_failed_label;
GtkWidget      *stats_rate_label;
GtkWidget      *stats_enrolled_label;
GtkWidget      *stats_total_seats_label;
GtkWidget      *chart_drawing_area;

/* Active nav button tracking */
GtkWidget      *nav_buttons[3];
int             active_nav = 0;

/* Header clock label */
GtkWidget      *header_clock_label;
GtkWidget      *header_status_label;

/* Animated stat values (for smooth transitions) */
int             anim_success_target = 0;
int             anim_failed_target  = 0;

GtkListStore   *course_store;
GtkWidget      *course_tree_view;

GtkListStore   *log_store;
GtkTreeView    *log_tree_view;
GtkTreeModel   *log_filter;
GtkEntry       *log_search_entry;

GtkWidget      *control_box;
GtkWidget      *spinner;
GtkWidget      *clear_log_button;
GtkWidget      *btn_mandatory;
GtkWidget      *btn_stress;

/* Loading overlay */
GtkWidget      *overlay;
GtkWidget      *loading_revealer;

typedef struct {
    char timestamp[16];
    int studentId;
    char priority[8];
    char courseName[MAX_NAME_LEN];
    char result[32];
    int result_type;
} LogEntry;

/* ==================== BACKEND SIMULATION (UNCHANGED) ==================== */
static int isAlreadyEnrolled(const Course *course, int studentId) {
    for (int i = 0; i < course->enrolledCount; i++)
        if (course->enrolledIds[i] == studentId) return 1;
    return 0;
}

static int tryRegister(int courseIndex, int studentId) {
    if (courseIndex < 0 || courseIndex >= numCourses) return -2;
    Course *course = &courses[courseIndex];
    int result;
    pthread_mutex_lock(&course->seatMutex);
    if (isAlreadyEnrolled(course, studentId)) result = -1;
    else if (course->availableSeats <= 0) result = 0;
    else {
        course->availableSeats--;
        course->enrolledIds[course->enrolledCount] = studentId;
        course->enrolledCount++;
        result = 1;
    }
    pthread_mutex_unlock(&course->seatMutex);
    return result;
}

static int pqInit(PriorityQueue *pq) {
    if (!pq) return -1;
    pq->head = NULL;
    pq->size = 0;
    return pthread_mutex_init(&pq->pqMutex, NULL);
}

static int pqEnqueue(PriorityQueue *pq, StudentRequest *request) {
    PQNode *node = malloc(sizeof(PQNode));
    if (!node) return -1;
    node->request = request;
    node->next = NULL;
    pthread_mutex_lock(&pq->pqMutex);
    if (request->priority == HIGH_PRIORITY) {
        node->next = pq->head;
        pq->head = node;
    } else {
        if (!pq->head) pq->head = node;
        else {
            PQNode *tail = pq->head;
            while (tail->next) tail = tail->next;
            tail->next = node;
        }
    }
    pq->size++;
    pthread_mutex_unlock(&pq->pqMutex);
    return 0;
}

static StudentRequest *pqDequeue(PriorityQueue *pq) {
    if (!pq) return NULL;
    pthread_mutex_lock(&pq->pqMutex);
    if (!pq->head) {
        pthread_mutex_unlock(&pq->pqMutex);
        return NULL;
    }
    PQNode *node = pq->head;
    pq->head = node->next;
    StudentRequest *req = node->request;
    pq->size--;
    free(node);
    pthread_mutex_unlock(&pq->pqMutex);
    return req;
}

static void pqDestroy(PriorityQueue *pq) {
    StudentRequest *req;
    while ((req = pqDequeue(pq))) free(req);
    pthread_mutex_destroy(&pq->pqMutex);
}

static gboolean idle_add_log_entry(gpointer data) {
    LogEntry *entry = (LogEntry*)data;
    GtkTreeIter iter;
    gtk_list_store_append(log_store, &iter);
    gtk_list_store_set(log_store, &iter,
        0, entry->timestamp,
        1, entry->studentId,
        2, entry->priority,
        3, entry->courseName,
        4, entry->result,
        5, entry->result_type,
        -1);

    /* Auto-scroll to newest entry */
    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(log_store), &iter);
    if (path) {
        gtk_tree_view_scroll_to_cell(log_tree_view, path, NULL, FALSE, 0, 0);
        gtk_tree_path_free(path);
    }

    free(entry);
    return G_SOURCE_REMOVE;
}

static void *studentThread(void *arg) {
    StudentRequest *request = (StudentRequest*)arg;
    int studentId = request->studentId;
    int priority = request->priority;
    if (priority == LOW_PRIORITY) {
        struct timespec delayTs = {0, (long)PRIORITY_DELAY_US * 1000L};
        nanosleep(&delayTs, NULL);
    }
    for (int i = 0; i < request->numCoursesToTry; i++) {
        int idx = request->courseIndices[i];
        if (idx < 0 || idx >= numCourses) continue;
        int regResult = tryRegister(idx, studentId);
        const char *result_str;
        int result_type;
        switch (regResult) {
            case 1:  result_str = "SUCCESS"; result_type = 0; break;
            case 0:  result_str = "No Seats"; result_type = 1; break;
            case -1: result_str = "Already Enrolled"; result_type = 1; break;
            default: result_str = "Invalid Course"; result_type = 2; break;
        }

        pthread_mutex_lock(&logMutex);
        if (logFp) {
            char timestamp[16];
            time_t raw = time(NULL);
            struct tm *tmInfo = localtime(&raw);
            strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tmInfo);
            fprintf(logFp, "[%s] Student %3d | Priority: %s | Course: %-6s | %s\n",
                    timestamp, studentId,
                    (priority == HIGH_PRIORITY) ? "HIGH" : "LOW",
                    courses[idx].courseName, result_str);
            fflush(logFp);
        }
        pthread_mutex_unlock(&logMutex);

        char timestamp[16];
        time_t raw = time(NULL);
        struct tm *tmInfo = localtime(&raw);
        strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tmInfo);
        const char *priority_str = (priority == HIGH_PRIORITY) ? "High" : "Low";
        LogEntry *entry = malloc(sizeof(LogEntry));
        strcpy(entry->timestamp, timestamp);
        entry->studentId = studentId;
        strcpy(entry->priority, priority_str);
        strcpy(entry->courseName, courses[idx].courseName);
        strcpy(entry->result, result_str);
        entry->result_type = result_type;
        g_idle_add(idle_add_log_entry, entry);

        pthread_mutex_lock(&statsMutex);
        if (regResult == 1) totalSuccess++;
        else totalFailed++;
        pthread_mutex_unlock(&statsMutex);
    }
    free(request);
    return NULL;
}

static int runScenario(int numStudents, int numHighPriority) {
    PriorityQueue pq;
    pthread_t threadIds[MAX_STUDENTS];
    int threadsCreated = 0;
    if (numStudents <= 0 || numStudents > MAX_STUDENTS) return -1;
    if (numHighPriority < 0 || numHighPriority > numStudents) return -1;
    if (numCourses <= 0) return -1;
    if (pqInit(&pq) != 0) return -1;
    for (int i = 0; i < numStudents; i++) {
        StudentRequest *req = malloc(sizeof(StudentRequest));
        if (!req) continue;
        req->studentId = i + 1;
        req->priority = (i < numHighPriority) ? HIGH_PRIORITY : LOW_PRIORITY;
        req->numCoursesToTry = 0;
        int numTry = 1 + rand() % MAX_COURSES_PER_STU;
        for (int j = 0; j < numTry; j++) {
            int candidate = rand() % numCourses;
            int dup = 0;
            for (int k = 0; k < req->numCoursesToTry; k++)
                if (req->courseIndices[k] == candidate) { dup = 1; break; }
            if (!dup) req->courseIndices[req->numCoursesToTry++] = candidate;
        }
        pqEnqueue(&pq, req);
    }
    StudentRequest *req;
    while ((req = pqDequeue(&pq)) != NULL) {
        if (pthread_create(&threadIds[threadsCreated], NULL, studentThread, req) != 0)
            free(req);
        else
            threadsCreated++;
    }
    for (int i = 0; i < threadsCreated; i++) pthread_join(threadIds[i], NULL);
    pqDestroy(&pq);
    return 0;
}

static void reset_simulation_state(void) {
    for (int i = 0; i < numCourses; i++) pthread_mutex_destroy(&courses[i].seatMutex);
    numCourses = 0;
    pthread_mutex_lock(&statsMutex);
    totalSuccess = totalFailed = 0;
    pthread_mutex_unlock(&statsMutex);
}

typedef struct {
    int total;
    int high;
    void (*setup_courses)(void);
} ScenarioParams;

static void setup_mandatory_courses(void) {
    static const struct { int id; const char *name; int seats; } spec[] = {
        {101, "CS101", 2}, {102, "CS102", 1}, {103, "CS103", 3}
    };
    numCourses = sizeof(spec)/sizeof(spec[0]);
    for (int i = 0; i < numCourses; i++) {
        courses[i].courseId = spec[i].id;
        courses[i].totalSeats = spec[i].seats;
        courses[i].availableSeats = spec[i].seats;
        courses[i].enrolledCount = 0;
        memset(courses[i].enrolledIds, 0, sizeof(courses[i].enrolledIds));
        strncpy(courses[i].courseName, spec[i].name, MAX_NAME_LEN-1);
        pthread_mutex_init(&courses[i].seatMutex, NULL);
    }
}

static void setup_stress_courses(void) {
    static const struct { int id; const char *name; int seats; } catalogue[] = {
        {101, "CS101", 5}, {102, "CS102", 3}, {103, "CS103", 8},
        {104, "CS104", 4}, {105, "CS105", 6}, {106, "CS106", 5},
        {107, "CS107", 7}, {108, "CS108", 3}
    };
    numCourses = sizeof(catalogue)/sizeof(catalogue[0]);
    for (int i = 0; i < numCourses; i++) {
        courses[i].courseId = catalogue[i].id;
        courses[i].totalSeats = catalogue[i].seats;
        courses[i].availableSeats = catalogue[i].seats;
        courses[i].enrolledCount = 0;
        memset(courses[i].enrolledIds, 0, sizeof(courses[i].enrolledIds));
        strncpy(courses[i].courseName, catalogue[i].name, MAX_NAME_LEN-1);
        pthread_mutex_init(&courses[i].seatMutex, NULL);
    }
}

/* ==================== PREMIUM CSS ==================== */
static void apply_premium_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        /* ─── Global reset / typography ─── */
        "* { "
        "  font-family: 'Inter', 'Segoe UI', 'Ubuntu', 'Cantarell', sans-serif;"
        "  font-size: 11pt;"
        "  -gtk-icon-style: symbolic;"
        "}"

        /* ─── Window ─── */
        "window {"
        "  background-color: #060d1a;"
        "}"

        /* ─── Header bar ─── */
        "headerbar {"
        "  background: linear-gradient(135deg, #0d1b2e 0%, #081122 100%);"
        "  border-bottom: 1px solid rgba(45,212,191,0.35);"
        "  min-height: 52px;"
        "  padding: 0 16px;"
        "  box-shadow: 0 2px 24px rgba(0,0,0,0.6);"
        "}"
        "headerbar .title {"
        "  font-size: 13pt;"
        "  font-weight: 700;"
        "  color: #e2e8f0;"
        "  letter-spacing: 0.04em;"
        "}"
        "headerbar .subtitle {"
        "  font-size: 8pt;"
        "  color: #2dd4bf;"
        "  letter-spacing: 0.06em;"
        "}"
        "headerbar button {"
        "  background: transparent;"
        "  border: none;"
        "  color: #94a3b8;"
        "  border-radius: 8px;"
        "  padding: 4px 8px;"
        "  min-height: 0;"
        "  min-width: 0;"
        "}"
        "headerbar button:hover {"
        "  background: rgba(45,212,191,0.12);"
        "  color: #2dd4bf;"
        "}"

        /* ─── Sidebar ─── */
        "#sidebar {"
        "  background: linear-gradient(180deg, #080f1d 0%, #050b18 100%);"
        "  border-right: 1px solid rgba(45,212,191,0.15);"
        "}"

        /* Logo area */
        "#logo-box {"
        "  background: transparent;"
        "  border-bottom: 1px solid rgba(45,212,191,0.12);"
        "  padding: 20px 16px 16px 16px;"
        "}"
        "#logo-title {"
        "  font-size: 13pt;"
        "  font-weight: 800;"
        "  color: #2dd4bf;"
        "  letter-spacing: 0.08em;"
        "}"
        "#logo-sub {"
        "  font-size: 7.5pt;"
        "  color: #475569;"
        "  letter-spacing: 0.12em;"
        "}"

        /* Nav buttons */
        ".nav-btn {"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 11px 14px;"
        "  margin: 2px 10px;"
        "  color: #64748b;"
        "  font-size: 10.5pt;"
        "  font-weight: 600;"
        "  letter-spacing: 0.02em;"
        "  transition: all 200ms ease;"
        "}"
        ".nav-btn:hover {"
        "  background: rgba(45,212,191,0.10);"
        "  color: #2dd4bf;"
        "}"
        ".nav-btn-active {"
        "  background: rgba(45,212,191,0.15);"
        "  color: #2dd4bf;"
        "  border-left: 3px solid #2dd4bf;"
        "}"

        /* Scenario buttons */
        "#btn-mandatory {"
        "  background: linear-gradient(135deg, #0ea5e9, #2563eb);"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 10px 14px;"
        "  color: #ffffff;"
        "  font-weight: 700;"
        "  font-size: 10pt;"
        "  box-shadow: 0 4px 14px rgba(14,165,233,0.35);"
        "  transition: all 200ms ease;"
        "}"
        "#btn-mandatory:hover {"
        "  background: linear-gradient(135deg, #38bdf8, #3b82f6);"
        "  box-shadow: 0 4px 20px rgba(14,165,233,0.55);"
        "}"
        "#btn-mandatory:active {"
        "  background: linear-gradient(135deg, #0284c7, #1d4ed8);"
        "  box-shadow: none;"
        "}"
        "#btn-mandatory:disabled {"
        "  opacity: 0.45;"
        "  box-shadow: none;"
        "}"

        "#btn-stress {"
        "  background: linear-gradient(135deg, #7c3aed, #4f46e5);"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 10px 14px;"
        "  color: #ffffff;"
        "  font-weight: 700;"
        "  font-size: 10pt;"
        "  box-shadow: 0 4px 14px rgba(124,58,237,0.35);"
        "  transition: all 200ms ease;"
        "}"
        "#btn-stress:hover {"
        "  background: linear-gradient(135deg, #8b5cf6, #6366f1);"
        "  box-shadow: 0 4px 20px rgba(124,58,237,0.55);"
        "}"
        "#btn-stress:active {"
        "  background: linear-gradient(135deg, #6d28d9, #4338ca);"
        "  box-shadow: none;"
        "}"
        "#btn-stress:disabled {"
        "  opacity: 0.45;"
        "  box-shadow: none;"
        "}"

        "#btn-clear {"
        "  background: rgba(30,41,59,0.7);"
        "  border: 1px solid rgba(248,113,113,0.35);"
        "  border-radius: 12px;"
        "  padding: 8px 12px;"
        "  color: #f87171;"
        "  font-weight: 600;"
        "  font-size: 10pt;"
        "  transition: all 200ms ease;"
        "}"
        "#btn-clear:hover {"
        "  background: rgba(248,113,113,0.15);"
        "  border-color: #f87171;"
        "  box-shadow: 0 0 12px rgba(248,113,113,0.3);"
        "}"
        "#btn-clear:disabled { opacity: 0.4; }"

        /* Status dot */
        "#status-dot {"
        "  color: #2dd4bf;"
        "  font-size: 9pt;"
        "}"
        "#clock-label {"
        "  font-size: 9pt;"
        "  font-weight: 600;"
        "  color: #64748b;"
        "  font-feature-settings: 'tnum';"
        "}"

        /* ─── Stat cards ─── */
        ".stat-card {"
        "  background: linear-gradient(145deg, rgba(15,23,42,0.9), rgba(8,15,32,0.95));"
        "  border-radius: 18px;"
        "  border: 1px solid rgba(45,212,191,0.12);"
        "  padding: 0px;"
        "}"
        ".stat-value {"
        "  font-size: 30pt;"
        "  font-weight: 800;"
        "  color: #2dd4bf;"
        "  letter-spacing: -0.02em;"
        "}"
        ".stat-value-red {"
        "  font-size: 30pt;"
        "  font-weight: 800;"
        "  color: #f87171;"
        "  letter-spacing: -0.02em;"
        "}"
        ".stat-value-yellow {"
        "  font-size: 30pt;"
        "  font-weight: 800;"
        "  color: #facc15;"
        "  letter-spacing: -0.02em;"
        "}"
        ".stat-value-green {"
        "  font-size: 30pt;"
        "  font-weight: 800;"
        "  color: #4ade80;"
        "  letter-spacing: -0.02em;"
        "}"
        ".stat-label {"
        "  font-size: 8.5pt;"
        "  font-weight: 600;"
        "  color: #475569;"
        "  letter-spacing: 0.1em;"
        "  text-transform: uppercase;"
        "}"
        ".stat-icon {"
        "  font-size: 18pt;"
        "  color: rgba(45,212,191,0.6);"
        "}"

        /* ─── Section header ─── */
        ".section-title {"
        "  font-size: 11pt;"
        "  font-weight: 700;"
        "  color: #94a3b8;"
        "  letter-spacing: 0.08em;"
        "}"
        ".section-divider {"
        "  background: rgba(45,212,191,0.15);"
        "  min-height: 1px;"
        "}"

        /* ─── Chart card ─── */
        "#chart-card {"
        "  background: linear-gradient(145deg, rgba(10,17,30,0.95), rgba(5,11,22,0.98));"
        "  border-radius: 18px;"
        "  border: 1px solid rgba(45,212,191,0.12);"
        "}"

        /* ─── TreeView ─── */
        "treeview {"
        "  background-color: #080f1d;"
        "  color: #cbd5e1;"
        "  border-radius: 0;"
        "  outline: none;"
        "}"
        "treeview:selected {"
        "  background-color: rgba(45,212,191,0.2);"
        "  color: #2dd4bf;"
        "}"
        "treeview header button {"
        "  background: #0d1b2e;"
        "  border-bottom: 1px solid rgba(45,212,191,0.2);"
        "  color: #64748b;"
        "  font-size: 8.5pt;"
        "  font-weight: 700;"
        "  letter-spacing: 0.08em;"
        "  padding: 8px 10px;"
        "  border-radius: 0;"
        "  min-height: 0;"
        "}"
        "treeview header button:hover {"
        "  background: rgba(45,212,191,0.08);"
        "  color: #2dd4bf;"
        "}"
        ".table-card {"
        "  background: linear-gradient(145deg, rgba(10,17,30,0.95), rgba(5,11,22,0.98));"
        "  border-radius: 18px;"
        "  border: 1px solid rgba(45,212,191,0.12);"
        "}"

        /* ─── Progress bar ─── */
        "progressbar {"
        "  min-height: 8px;"
        "}"
        "progressbar trough {"
        "  background: rgba(30,41,59,0.6);"
        "  border-radius: 4px;"
        "  min-height: 8px;"
        "  border: none;"
        "}"
        "progressbar progress {"
        "  background: linear-gradient(90deg, #0ea5e9, #2dd4bf);"
        "  border-radius: 4px;"
        "  min-height: 8px;"
        "}"

        /* ─── Scrollbar ─── */
        "scrollbar {"
        "  background: transparent;"
        "  border: none;"
        "}"
        "scrollbar slider {"
        "  background: rgba(45,212,191,0.2);"
        "  border-radius: 4px;"
        "  min-width: 4px;"
        "  min-height: 4px;"
        "  margin: 2px;"
        "}"
        "scrollbar slider:hover {"
        "  background: rgba(45,212,191,0.4);"
        "}"
        "scrolledwindow {"
        "  background: transparent;"
        "}"

        /* ─── Search entry ─── */
        "#log-search {"
        "  background: rgba(8,15,29,0.9);"
        "  border: 1px solid rgba(45,212,191,0.25);"
        "  border-radius: 14px;"
        "  padding: 8px 14px;"
        "  color: #e2e8f0;"
        "  font-size: 10pt;"
        "  caret-color: #2dd4bf;"
        "}"
        "#log-search:focus {"
        "  border-color: rgba(45,212,191,0.6);"
        "  box-shadow: 0 0 0 2px rgba(45,212,191,0.12);"
        "  outline: none;"
        "}"
        "#log-search placeholder {"
        "  color: #334155;"
        "}"

        /* ─── Loading overlay ─── */
        "#loading-box {"
        "  background: rgba(6,13,26,0.88);"
        "  border-radius: 16px;"
        "  border: 1px solid rgba(45,212,191,0.25);"
        "  padding: 24px 36px;"
        "}"
        "#loading-label {"
        "  color: #2dd4bf;"
        "  font-size: 11pt;"
        "  font-weight: 600;"
        "  letter-spacing: 0.06em;"
        "}"

        /* ─── Spinner ─── */
        "spinner {"
        "  color: #2dd4bf;"
        "}"

        /* ─── Stack ─── */
        "stack {"
        "  background-color: #060d1a;"
        "}";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ==================== GUI HELPERS & IDLE CALLBACKS ==================== */
typedef struct {
    GtkWidget *widget;
    gboolean   sensitive;
} SensitiveData;

static gboolean idle_set_sensitive(gpointer data) {
    SensitiveData *sd = (SensitiveData*)data;
    gtk_widget_set_sensitive(sd->widget, sd->sensitive);
    free(sd);
    return G_SOURCE_REMOVE;
}

static gboolean idle_set_button_insensitive(gpointer data) {
    gtk_widget_set_sensitive(GTK_WIDGET(data), FALSE);
    return G_SOURCE_REMOVE;
}

static gboolean idle_set_button_sensitive(gpointer data) {
    gtk_widget_set_sensitive(GTK_WIDGET(data), TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean idle_clear_log(gpointer unused) {
    gtk_list_store_clear(log_store);
    return G_SOURCE_REMOVE;
}

static gboolean idle_update_course_table(gpointer unused) {
    GtkTreeIter iter;
    gtk_list_store_clear(course_store);
    for (int i = 0; i < numCourses; i++) {
        int enrolled = courses[i].totalSeats - courses[i].availableSeats;
        float percent = (courses[i].totalSeats > 0) ? (enrolled * 100.0f / courses[i].totalSeats) : 0.0f;
        gtk_list_store_append(course_store, &iter);
        gtk_list_store_set(course_store, &iter,
            0, courses[i].courseId,
            1, courses[i].courseName,
            2, courses[i].totalSeats,
            3, courses[i].availableSeats,
            4, enrolled,
            5, percent,
            -1);
    }
    if (chart_drawing_area)
        gtk_widget_queue_draw(chart_drawing_area);
    return G_SOURCE_REMOVE;
}

static gboolean idle_update_dashboard(gpointer unused) {
    int totalEnrolled = 0;
    int totalSeats = 0;
    for (int i = 0; i < numCourses; i++) {
        totalSeats += courses[i].totalSeats;
        totalEnrolled += (courses[i].totalSeats - courses[i].availableSeats);
    }
    float rate = (totalSuccess + totalFailed) > 0 ?
                 (totalSuccess * 100.0f / (totalSuccess + totalFailed)) : 0.0f;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", totalSuccess);
    gtk_label_set_text(GTK_LABEL(stats_success_label), buf);
    snprintf(buf, sizeof(buf), "%d", totalFailed);
    gtk_label_set_text(GTK_LABEL(stats_failed_label), buf);
    snprintf(buf, sizeof(buf), "%.1f%%", rate);
    gtk_label_set_text(GTK_LABEL(stats_rate_label), buf);
    snprintf(buf, sizeof(buf), "%d", totalEnrolled);
    gtk_label_set_text(GTK_LABEL(stats_enrolled_label), buf);
    snprintf(buf, sizeof(buf), "%d", totalSeats);
    gtk_label_set_text(GTK_LABEL(stats_total_seats_label), buf);
    return G_SOURCE_REMOVE;
}

/* ─── Helper: draw rounded rectangle ─── */
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    double d = r * 2.0;
    cairo_new_path(cr);
    cairo_arc(cr, x + r,     y + r,     r, M_PI,       3*M_PI/2);
    cairo_arc(cr, x + w - r, y + r,     r, 3*M_PI/2,   0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          M_PI/2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI/2,     M_PI);
    cairo_close_path(cr);
    (void)d;
}

/* ─── Premium chart with gradient bars, grid, labels ─── */
static gboolean draw_chart(GtkWidget *widget, cairo_t *cr, gpointer data) {
    if (numCourses == 0) return FALSE;

    int width  = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    /* Anti-aliasing */
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    /* Background */
    cairo_set_source_rgba(cr, 0.023, 0.063, 0.113, 1.0);
    cairo_paint(cr);

    /* Padding */
    int pad_left = 48, pad_right = 20, pad_top = 36, pad_bottom = 52;
    int chart_w = width  - pad_left - pad_right;
    int chart_h = height - pad_top  - pad_bottom;

    /* Grid lines */
    int grid_lines = 5;
    cairo_set_line_width(cr, 0.5);
    for (int g = 0; g <= grid_lines; g++) {
        double y = pad_top + chart_h - (double)g / grid_lines * chart_h;
        cairo_set_source_rgba(cr, 0.176, 0.255, 0.369, 0.4);
        cairo_move_to(cr, pad_left, y);
        cairo_line_to(cr, width - pad_right, y);
        cairo_stroke(cr);

        /* Y-axis labels */
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", g * 20);
        cairo_set_source_rgba(cr, 0.361, 0.439, 0.557, 1.0);
        cairo_set_font_size(cr, 8.5);
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_text_extents_t te;
        cairo_text_extents(cr, buf, &te);
        cairo_move_to(cr, pad_left - te.width - 8, y + te.height / 2.0);
        cairo_show_text(cr, buf);
    }

    /* Axes */
    cairo_set_source_rgba(cr, 0.176, 0.255, 0.369, 0.8);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, pad_left, pad_top);
    cairo_line_to(cr, pad_left, pad_top + chart_h);
    cairo_line_to(cr, pad_left + chart_w, pad_top + chart_h);
    cairo_stroke(cr);

    /* Bars */
    int n = numCourses;
    double slot_w = (double)chart_w / n;
    double bar_w  = slot_w * 0.55;
    double bar_gap = (slot_w - bar_w) / 2.0;

    for (int i = 0; i < n; i++) {
        int enrolled = courses[i].totalSeats - courses[i].availableSeats;
        double percent = (courses[i].totalSeats > 0)
            ? (enrolled * 100.0 / courses[i].totalSeats) : 0.0;
        double bar_h = (percent / 100.0) * chart_h;
        if (bar_h < 2.0) bar_h = 2.0;

        double bx = pad_left + i * slot_w + bar_gap;
        double by = pad_top + chart_h - bar_h;

        /* Glow shadow */
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.176, 0.831, 0.749, 0.12);
        rounded_rect(cr, bx - 3, by - 3, bar_w + 6, bar_h + 6, 5);
        cairo_fill(cr);
        cairo_restore(cr);

        /* Gradient fill */
        cairo_pattern_t *pat = cairo_pattern_create_linear(bx, by, bx, by + bar_h);
        if (percent >= 80.0) {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.973, 0.506, 0.424, 1.0);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.788, 0.212, 0.212, 0.8);
        } else if (percent >= 50.0) {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.980, 0.800, 0.082, 1.0);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.780, 0.500, 0.020, 0.8);
        } else {
            cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.176, 0.831, 0.749, 1.0);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.055, 0.647, 0.914, 0.8);
        }
        rounded_rect(cr, bx, by, bar_w, bar_h, 5);
        cairo_set_source(cr, pat);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);

        /* Top highlight line */
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, bx + 4, by + 1);
        cairo_line_to(cr, bx + bar_w - 4, by + 1);
        cairo_stroke(cr);

        /* Percentage label above bar */
        char pct_label[8];
        snprintf(pct_label, sizeof(pct_label), "%.0f%%", percent);
        cairo_set_font_size(cr, 8.5);
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_text_extents_t te;
        cairo_text_extents(cr, pct_label, &te);
        cairo_set_source_rgba(cr, 0.882, 0.914, 0.941, 0.9);
        cairo_move_to(cr, bx + (bar_w - te.width) / 2.0, by - 5);
        cairo_show_text(cr, pct_label);

        /* Enrolled count inside bar (if tall enough) */
        if (bar_h > 22) {
            char cnt_label[8];
            snprintf(cnt_label, sizeof(cnt_label), "%d", enrolled);
            cairo_text_extents(cr, cnt_label, &te);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
            cairo_move_to(cr, bx + (bar_w - te.width) / 2.0, by + 16);
            cairo_show_text(cr, cnt_label);
        }

        /* X-axis course label */
        cairo_set_source_rgba(cr, 0.502, 0.588, 0.710, 1.0);
        cairo_set_font_size(cr, 8.5);
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_text_extents(cr, courses[i].courseName, &te);
        cairo_move_to(cr,
            bx + (bar_w - te.width) / 2.0,
            pad_top + chart_h + 16);
        cairo_show_text(cr, courses[i].courseName);
    }

    /* Chart title */
    cairo_set_font_size(cr, 10.0);
    cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_source_rgba(cr, 0.502, 0.588, 0.710, 0.9);

    const char *title = "SEAT OCCUPANCY BY COURSE";
    cairo_text_extents_t tte;
    cairo_text_extents(cr, title, &tte);
    cairo_move_to(cr, (width - tte.width) / 2.0, pad_top - 14);
    cairo_show_text(cr, title);

    return FALSE;
}

/* ==================== NAV ACTIVE STATE ==================== */
static void set_active_nav(int idx) {
    for (int i = 0; i < 3; i++) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(nav_buttons[i]);
        gtk_style_context_remove_class(ctx, "nav-btn-active");
        gtk_style_context_add_class(ctx, "nav-btn");
    }
    GtkStyleContext *ctx = gtk_widget_get_style_context(nav_buttons[idx]);
    gtk_style_context_add_class(ctx, "nav-btn-active");
    active_nav = idx;
}

static void on_nav_clicked(GtkButton *button, gpointer page_name) {
    gtk_stack_set_visible_child_name(GTK_STACK(stack), (char*)page_name);
    intptr_t idx = (intptr_t)g_object_get_data(G_OBJECT(button), "nav-index");
    set_active_nav((int)idx);
}

/* ==================== LOG FILTER ==================== */
static gboolean log_filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data) {
    const char *search_text = gtk_entry_get_text(GTK_ENTRY(log_search_entry));
    if (!search_text || strlen(search_text) == 0) return TRUE;

    gchar *ts = NULL, *pri = NULL, *course = NULL, *result = NULL;
    gint studentId = 0;
    gtk_tree_model_get(model, iter,
        0, &ts, 1, &studentId, 2, &pri, 3, &course, 4, &result, -1);

    char studentIdStr[16];
    snprintf(studentIdStr, sizeof(studentIdStr), "%d", studentId);

    char haystack[512];
    snprintf(haystack, sizeof(haystack), "%s %s %s %s %s",
             ts ? ts : "", studentIdStr,
             pri ? pri : "", course ? course : "", result ? result : "");
    g_free(ts); g_free(pri); g_free(course); g_free(result);

    return (strcasestr(haystack, search_text) != NULL);
}

static void on_search_changed(GtkEntry *entry, gpointer data) {
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(log_filter));
}

/* ==================== CELL RENDERERS ==================== */
static void result_cell_data_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                                  GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    gint type = 0;
    gchar *result_text = NULL;
    gtk_tree_model_get(model, iter, 5, &type, 4, &result_text, -1);

    const char *fg  = (type == 0) ? "#2dd4bf" : (type == 1) ? "#facc15" : "#f87171";
    const char *bg  = (type == 0) ? "#0a2218" : (type == 1) ? "#201a04" : "#220a0a";
    const char *wt  = (type == 0) ? "bold" : "normal";

    /* Build padded badge text */
    char badge[64];
    snprintf(badge, sizeof(badge), "  %s  ", result_text ? result_text : "");
    g_free(result_text);

    g_object_set(renderer,
        "foreground", fg,
        "background", bg,
        "text", badge,
        "weight-set", TRUE,
        "foreground-set", TRUE,
        "background-set", TRUE,
        NULL);
    (void)wt;
}

static void priority_cell_data_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                                    GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    gchar *pri = NULL;
    gtk_tree_model_get(model, iter, 2, &pri, -1);
    if (pri && strcmp(pri, "High") == 0) {
        g_object_set(renderer, "foreground", "#fb923c",
                     "foreground-set", TRUE, NULL);
    } else {
        g_object_set(renderer, "foreground", "#64748b",
                     "foreground-set", TRUE, NULL);
    }
    g_free(pri);
}

/* ==================== LIVE CLOCK ==================== */
static gboolean update_clock(gpointer data) {
    if (!header_clock_label) return G_SOURCE_REMOVE;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", t);
    gtk_label_set_text(GTK_LABEL(header_clock_label), buf);
    return G_SOURCE_CONTINUE;
}

/* ==================== STAT CARD BUILDER ==================== */
static GtkWidget* build_stat_card(const char *icon, const char *label,
                                  GtkWidget **out_value_label,
                                  const char *value_class) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkStyleContext *ctx = gtk_widget_get_style_context(card);
    gtk_style_context_add_class(ctx, "stat-card");
    gtk_widget_set_margin_start(card, 0);
    gtk_widget_set_margin_end(card, 0);
    gtk_widget_set_hexpand(card, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(card), 20);

    /* Top row: icon + label */
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon_lbl = gtk_label_new(icon);
    gtk_style_context_add_class(gtk_widget_get_style_context(icon_lbl), "stat-icon");
    gtk_box_pack_start(GTK_BOX(top_row), icon_lbl, FALSE, FALSE, 0);

    GtkWidget *title_lbl = gtk_label_new(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "stat-label");
    gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top_row), title_lbl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), top_row, FALSE, FALSE, 0);

    /* Value */
    GtkWidget *val = gtk_label_new("0");
    gtk_style_context_add_class(gtk_widget_get_style_context(val), value_class);
    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), val, FALSE, FALSE, 0);
    *out_value_label = val;

    return card;
}

/* ==================== PAGE BUILDERS ==================== */
static GtkWidget* create_dashboard_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(page), 24);

    /* Section header */
    GtkWidget *sec_label = gtk_label_new("OVERVIEW");
    gtk_style_context_add_class(gtk_widget_get_style_context(sec_label), "section-title");
    gtk_widget_set_halign(sec_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), sec_label, FALSE, FALSE, 0);

    /* Stats grid – 5 cards in 2 rows */
    GtkWidget *stats_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(stats_grid), 14);
    gtk_grid_set_column_spacing(GTK_GRID(stats_grid), 14);
    gtk_box_pack_start(GTK_BOX(page), stats_grid, FALSE, FALSE, 0);

    /* Row 0: Success | Failed | Rate */
    GtkWidget *c0 = build_stat_card("✔", "SUCCESSFUL",   &stats_success_label, "stat-value");
    GtkWidget *c1 = build_stat_card("✘", "FAILED",        &stats_failed_label,  "stat-value-red");
    GtkWidget *c2 = build_stat_card("◎", "SUCCESS RATE",  &stats_rate_label,    "stat-value-yellow");
    gtk_grid_attach(GTK_GRID(stats_grid), c0, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stats_grid), c1, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stats_grid), c2, 2, 0, 1, 1);

    /* Row 1: Enrolled | Total Seats */
    GtkWidget *c3 = build_stat_card("▦", "ENROLLED",    &stats_enrolled_label,    "stat-value-green");
    GtkWidget *c4 = build_stat_card("□", "TOTAL SEATS", &stats_total_seats_label, "stat-value");
    gtk_grid_attach(GTK_GRID(stats_grid), c3, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(stats_grid), c4, 1, 1, 1, 1);

    /* Divider */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(sep), "section-divider");
    gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, 0);

    /* Chart section header */
    GtkWidget *chart_sec = gtk_label_new("SEAT ANALYTICS");
    gtk_style_context_add_class(gtk_widget_get_style_context(chart_sec), "section-title");
    gtk_widget_set_halign(chart_sec, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), chart_sec, FALSE, FALSE, 0);

    /* Chart card */
    GtkWidget *chart_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(chart_card, "chart-card");
    gtk_container_set_border_width(GTK_CONTAINER(chart_card), 4);

    chart_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(chart_drawing_area, -1, 260);
    g_signal_connect(chart_drawing_area, "draw", G_CALLBACK(draw_chart), NULL);
    gtk_box_pack_start(GTK_BOX(chart_card), chart_drawing_area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), chart_card, TRUE, TRUE, 0);

    return page;
}

static GtkWidget* create_courses_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(page), 24);

    /* Header */
    GtkWidget *header_lbl = gtk_label_new("COURSE REGISTRY");
    gtk_style_context_add_class(gtk_widget_get_style_context(header_lbl), "section-title");
    gtk_widget_set_halign(header_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), header_lbl, FALSE, FALSE, 0);

    /* Table card wrapper */
    GtkWidget *table_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(table_card, "table-card");

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_NONE);

    course_store = gtk_list_store_new(6, G_TYPE_INT, G_TYPE_STRING,
                                      G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_FLOAT);
    course_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(course_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(course_tree_view), TRUE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(course_tree_view), FALSE);
    //gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(course_tree_view), TRUE);

    /* Columns */
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "foreground", "#64748b",
                 "foreground-set", TRUE,
                 "xpad", 12, "ypad", 10, NULL);

    GtkCellRenderer *val_renderer = gtk_cell_renderer_text_new();
    g_object_set(val_renderer, "foreground", "#cbd5e1",
                 "foreground-set", TRUE,
                 "xpad", 12, "ypad", 10, NULL);

    GtkTreeViewColumn *col;

    col = gtk_tree_view_column_new_with_attributes("ID", renderer, "text", 0, NULL);
    gtk_tree_view_column_set_min_width(col, 60);
    gtk_tree_view_column_set_sort_column_id(col, 0);
    gtk_tree_view_append_column(GTK_TREE_VIEW(course_tree_view), col);

    col = gtk_tree_view_column_new_with_attributes("COURSE", val_renderer, "text", 1, NULL);
    gtk_tree_view_column_set_min_width(col, 120);
    gtk_tree_view_column_set_sort_column_id(col, 1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(course_tree_view), col);

    col = gtk_tree_view_column_new_with_attributes("TOTAL", renderer, "text", 2, NULL);
    gtk_tree_view_column_set_min_width(col, 70);
    gtk_tree_view_column_set_sort_column_id(col, 2);
    gtk_tree_view_append_column(GTK_TREE_VIEW(course_tree_view), col);

    col = gtk_tree_view_column_new_with_attributes("AVAIL", renderer, "text", 3, NULL);
    gtk_tree_view_column_set_min_width(col, 70);
    gtk_tree_view_column_set_sort_column_id(col, 3);
    gtk_tree_view_append_column(GTK_TREE_VIEW(course_tree_view), col);

    col = gtk_tree_view_column_new_with_attributes("ENROLLED", renderer, "text", 4, NULL);
    gtk_tree_view_column_set_min_width(col, 80);
    gtk_tree_view_column_set_sort_column_id(col, 4);
    gtk_tree_view_append_column(GTK_TREE_VIEW(course_tree_view), col);

    GtkCellRenderer *progress = gtk_cell_renderer_progress_new();
    g_object_set(progress, "xpad", 12, "ypad", 8, NULL);
    col = gtk_tree_view_column_new_with_attributes("OCCUPANCY", progress, "value", 5, NULL);
    gtk_tree_view_column_set_min_width(col, 140);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(course_tree_view), col);

    gtk_container_add(GTK_CONTAINER(scrolled), course_tree_view);
    gtk_box_pack_start(GTK_BOX(table_card), scrolled, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), table_card, TRUE, TRUE, 0);

    return page;
}

static GtkWidget* create_logs_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(page), 24);

    /* Header row */
    GtkWidget *hdr_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *header_lbl = gtk_label_new("REGISTRATION LOG");
    gtk_style_context_add_class(gtk_widget_get_style_context(header_lbl), "section-title");
    gtk_widget_set_halign(header_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(header_lbl, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(hdr_row), header_lbl, FALSE, FALSE, 0);

    /* Search entry with icon prefix */
    GtkWidget *search_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    log_search_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(GTK_ENTRY(log_search_entry), "  Search logs...");
    gtk_widget_set_name(GTK_WIDGET(log_search_entry), "log-search");
    gtk_widget_set_size_request(GTK_WIDGET(log_search_entry), 280, -1);
    gtk_box_pack_start(GTK_BOX(search_wrapper), GTK_WIDGET(log_search_entry), FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(hdr_row), search_wrapper, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), hdr_row, FALSE, FALSE, 0);

    /* Log table card */
    GtkWidget *table_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(table_card, "table-card");

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_NONE);

    log_store = gtk_list_store_new(6, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    log_tree_view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(log_store)));
    gtk_tree_view_set_headers_visible(log_tree_view, TRUE);
    gtk_tree_view_set_headers_clickable(log_tree_view, TRUE);
    // gtk_tree_view_set_rules_hint(log_tree_view, TRUE);

    /* Timestamp */
    GtkCellRenderer *ts_renderer = gtk_cell_renderer_text_new();
    g_object_set(ts_renderer, "foreground", "#475569",
                 "foreground-set", TRUE,
                 "font", "Monospace 9",
                 "xpad", 12, "ypad", 9, NULL);
    GtkTreeViewColumn *ts_col = gtk_tree_view_column_new_with_attributes(
        "TIME", ts_renderer, "text", 0, NULL);
    gtk_tree_view_column_set_min_width(ts_col, 90);
    gtk_tree_view_column_set_sort_column_id(ts_col, 0);
    gtk_tree_view_append_column(log_tree_view, ts_col);

    /* Student ID */
    GtkCellRenderer *id_renderer = gtk_cell_renderer_text_new();
    g_object_set(id_renderer, "foreground", "#94a3b8",
                 "foreground-set", TRUE,
                 "xpad", 12, "ypad", 9, NULL);
    GtkTreeViewColumn *id_col = gtk_tree_view_column_new_with_attributes(
        "STUDENT", id_renderer, "text", 1, NULL);
    gtk_tree_view_column_set_min_width(id_col, 80);
    gtk_tree_view_column_set_sort_column_id(id_col, 1);
    gtk_tree_view_append_column(log_tree_view, id_col);

    /* Priority with color func */
    GtkCellRenderer *pri_renderer = gtk_cell_renderer_text_new();
    g_object_set(pri_renderer, "xpad", 12, "ypad", 9, NULL);
    GtkTreeViewColumn *pri_col = gtk_tree_view_column_new_with_attributes(
        "PRIORITY", pri_renderer, "text", 2, NULL);
    gtk_tree_view_column_set_min_width(pri_col, 90);
    gtk_tree_view_column_set_cell_data_func(pri_col, pri_renderer,
        priority_cell_data_func, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(pri_col, 2);
    gtk_tree_view_append_column(log_tree_view, pri_col);

    /* Course */
    GtkCellRenderer *course_renderer = gtk_cell_renderer_text_new();
    g_object_set(course_renderer, "foreground", "#cbd5e1",
                 "foreground-set", TRUE,
                 "xpad", 12, "ypad", 9, NULL);
    GtkTreeViewColumn *course_col = gtk_tree_view_column_new_with_attributes(
        "COURSE", course_renderer, "text", 3, NULL);
    gtk_tree_view_column_set_min_width(course_col, 90);
    gtk_tree_view_column_set_sort_column_id(course_col, 3);
    gtk_tree_view_append_column(log_tree_view, course_col);

    /* Result with badge coloring */
    GtkCellRenderer *result_renderer = gtk_cell_renderer_text_new();
    g_object_set(result_renderer, "xpad", 8, "ypad", 9, NULL);
    GtkTreeViewColumn *result_col = gtk_tree_view_column_new_with_attributes(
        "STATUS", result_renderer, "text", 4, NULL);
    gtk_tree_view_column_set_min_width(result_col, 130);
    gtk_tree_view_column_set_expand(result_col, TRUE);
    gtk_tree_view_column_set_cell_data_func(result_col, result_renderer,
                                            result_cell_data_func, NULL, NULL);
    gtk_tree_view_append_column(log_tree_view, result_col);

    /* Filter model */
    log_filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(log_store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(log_filter),
                                           log_filter_func, NULL, NULL);
    gtk_tree_view_set_model(log_tree_view, log_filter);
    g_signal_connect(log_search_entry, "changed", G_CALLBACK(on_search_changed), NULL);

    gtk_container_add(GTK_CONTAINER(scrolled), GTK_WIDGET(log_tree_view));
    gtk_box_pack_start(GTK_BOX(table_card), scrolled, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), table_card, TRUE, TRUE, 0);

    return page;
}

/* ==================== LOADING OVERLAY ==================== */
static gboolean idle_show_loading(gpointer data) {
    if (loading_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(loading_revealer), TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean idle_hide_loading(gpointer data) {
    if (loading_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(loading_revealer), FALSE);
    return G_SOURCE_REMOVE;
}

/* ==================== SCENARIO THREAD ==================== */
static void* run_scenario_thread(void *params) {
    ScenarioParams *p = (ScenarioParams*)params;
    p->setup_courses();

    g_idle_add(idle_show_loading, NULL);
    g_idle_add((GSourceFunc)gtk_widget_show, spinner);
    g_idle_add((GSourceFunc)gtk_spinner_start, spinner);
    g_idle_add(idle_set_button_insensitive, clear_log_button);

    runScenario(p->total, p->high);

    g_idle_add(idle_update_course_table, NULL);
    g_idle_add(idle_update_dashboard, NULL);
    g_idle_add((GSourceFunc)gtk_spinner_stop, spinner);
    g_idle_add((GSourceFunc)gtk_widget_hide, spinner);
    g_idle_add(idle_set_button_sensitive, clear_log_button);
    g_idle_add(idle_hide_loading, NULL);

    SensitiveData *sd = malloc(sizeof(SensitiveData));
    sd->widget = control_box;
    sd->sensitive = TRUE;
    g_idle_add(idle_set_sensitive, sd);
    free(p);
    return NULL;
}

static void on_mandatory_clicked(GtkButton *button, gpointer data) {
    SensitiveData *sd = malloc(sizeof(SensitiveData));
    sd->widget = control_box;
    sd->sensitive = FALSE;
    g_idle_add(idle_set_sensitive, sd);
    ScenarioParams *params = malloc(sizeof(ScenarioParams));
    params->total = 10;
    params->high = 3;
    params->setup_courses = setup_mandatory_courses;
    pthread_t tid;
    pthread_create(&tid, NULL, run_scenario_thread, params);
    pthread_detach(tid);
}

static void on_stress_clicked(GtkButton *button, gpointer data) {
    SensitiveData *sd = malloc(sizeof(SensitiveData));
    sd->widget = control_box;
    sd->sensitive = FALSE;
    g_idle_add(idle_set_sensitive, sd);
    ScenarioParams *params = malloc(sizeof(ScenarioParams));
    params->total = 100;
    params->high = 30;
    params->setup_courses = setup_stress_courses;
    pthread_t tid;
    pthread_create(&tid, NULL, run_scenario_thread, params);
    pthread_detach(tid);
}

static void on_clear_log_clicked(GtkButton *button, gpointer data) {
    g_idle_add(idle_clear_log, NULL);
}

/* ==================== BUILD GUI ==================== */
static void build_gui(void) {
    apply_premium_css();

    /* ─── Header bar ─── */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "COURSE REGISTRATION SYSTEM");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "PRIORITY SCHEDULING  ·  DEADLOCK-FREE");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);

    /* Clock in header */
    GtkWidget *clock_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *status_dot = gtk_label_new("●");
    gtk_widget_set_name(status_dot, "status-dot");
    header_clock_label = gtk_label_new("--:--:--");
    gtk_widget_set_name(header_clock_label, "clock-label");
    gtk_box_pack_start(GTK_BOX(clock_box), status_dot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(clock_box), header_clock_label, FALSE, FALSE, 0);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), clock_box);

    gtk_window_set_titlebar(GTK_WINDOW(main_window), header);

    /* Start clock timer */
    update_clock(NULL);
    g_timeout_add(1000, update_clock, NULL);

    /* ─── Root layout ─── */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    /* ─── Sidebar ─── */
    sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 220, -1);
    gtk_box_pack_start(GTK_BOX(main_box), sidebar, FALSE, FALSE, 0);

    /* Logo area */
    GtkWidget *logo_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_name(logo_box, "logo-box");
    gtk_container_set_border_width(GTK_CONTAINER(logo_box), 0);
    gtk_widget_set_margin_start(logo_box, 0);
    gtk_widget_set_margin_end(logo_box, 0);

    /* Fake padding by embedding in a frame */
    GtkWidget *logo_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(logo_inner), 0);
    gtk_widget_set_margin_start(logo_inner, 18);
    gtk_widget_set_margin_end(logo_inner, 18);
    gtk_widget_set_margin_top(logo_inner, 20);
    gtk_widget_set_margin_bottom(logo_inner, 16);

    GtkWidget *logo_title = gtk_label_new("⬡ REGIS");
    gtk_widget_set_name(logo_title, "logo-title");
    gtk_widget_set_halign(logo_title, GTK_ALIGN_START);
    GtkWidget *logo_sub = gtk_label_new("CONCURRENT SCHEDULER");
    gtk_widget_set_name(logo_sub, "logo-sub");
    gtk_widget_set_halign(logo_sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(logo_inner), logo_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(logo_inner), logo_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(logo_box), logo_inner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), logo_box, FALSE, FALSE, 0);

    /* Separator */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(sep), "section-divider");
    gtk_box_pack_start(GTK_BOX(sidebar), sep, FALSE, FALSE, 0);

    /* Nav buttons */
    gtk_widget_set_margin_top(sidebar, 0);
    GtkWidget *nav_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(nav_section, 12);
    gtk_widget_set_margin_bottom(nav_section, 8);
    gtk_box_pack_start(GTK_BOX(sidebar), nav_section, FALSE, FALSE, 0);

    const char *nav_icons[] = {"⊞", "≡", "◈"};
    const char *nav_labels[] = {"  Dashboard", "  Courses", "  Logs"};
    const char *page_names[] = {"dashboard", "courses", "logs"};
    for (int i = 0; i < 3; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s", nav_icons[i], nav_labels[i]);
        GtkWidget *btn = gtk_button_new_with_label(buf);
        GtkStyleContext *ctx = gtk_widget_get_style_context(btn);
        gtk_style_context_add_class(ctx, "nav-btn");
        gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(btn, TRUE);
        g_object_set_data(G_OBJECT(btn), "nav-index", (gpointer)(intptr_t)i);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_nav_clicked), (gpointer)page_names[i]);
        gtk_box_pack_start(GTK_BOX(nav_section), btn, FALSE, FALSE, 0);
        nav_buttons[i] = btn;
    }
    set_active_nav(0);

    /* Push controls to bottom */
    gtk_box_pack_start(GTK_BOX(sidebar), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), TRUE, TRUE, 0);

    /* Separator above controls */
    GtkWidget *ctrl_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(ctrl_sep), "section-divider");
    gtk_box_pack_start(GTK_BOX(sidebar), ctrl_sep, FALSE, FALSE, 0);

    /* Controls section */
    control_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(control_box), 14);

    GtkWidget *ctrl_title = gtk_label_new("RUN SCENARIO");
    gtk_style_context_add_class(gtk_widget_get_style_context(ctrl_title), "stat-label");
    gtk_widget_set_halign(ctrl_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(control_box), ctrl_title, FALSE, FALSE, 0);

    btn_mandatory = gtk_button_new_with_label("▶  Mandatory (10 / 3 High)");
    gtk_widget_set_name(btn_mandatory, "btn-mandatory");
    gtk_box_pack_start(GTK_BOX(control_box), btn_mandatory, FALSE, FALSE, 0);

    btn_stress = gtk_button_new_with_label("⚡  Stress Test (100 / 30 High)");
    gtk_widget_set_name(btn_stress, "btn-stress");
    gtk_box_pack_start(GTK_BOX(control_box), btn_stress, FALSE, FALSE, 0);

    /* Clear + spinner row */
    GtkWidget *misc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    clear_log_button = gtk_button_new_with_label("✕  Clear Log");
    gtk_widget_set_name(clear_log_button, "btn-clear");
    gtk_box_pack_start(GTK_BOX(misc_row), clear_log_button, TRUE, TRUE, 0);
    spinner = gtk_spinner_new();
    gtk_widget_set_size_request(spinner, 20, 20);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_hide(spinner);
    gtk_box_pack_start(GTK_BOX(misc_row), spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(control_box), misc_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(sidebar), control_box, FALSE, FALSE, 0);

    /* ─── Main content area with overlay ─── */
    overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(main_box), overlay, TRUE, TRUE, 0);

    /* Stack */
    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 280);
    gtk_container_add(GTK_CONTAINER(overlay), stack);

    /* Loading overlay revealer */
    loading_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(loading_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(loading_revealer), 200);
    gtk_widget_set_halign(loading_revealer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(loading_revealer, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), loading_revealer);

    GtkWidget *loading_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_name(loading_box, "loading-box");
    gtk_container_set_border_width(GTK_CONTAINER(loading_box), 0);
    gtk_widget_set_margin_start(loading_box, 24);
    gtk_widget_set_margin_end(loading_box, 24);
    gtk_widget_set_margin_top(loading_box, 18);
    gtk_widget_set_margin_bottom(loading_box, 18);

    GtkWidget *ov_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(ov_spinner, 22, 22);
    gtk_spinner_start(GTK_SPINNER(ov_spinner));
    gtk_box_pack_start(GTK_BOX(loading_box), ov_spinner, FALSE, FALSE, 0);

    GtkWidget *loading_lbl = gtk_label_new("Processing registrations...");
    gtk_widget_set_name(loading_lbl, "loading-label");
    gtk_box_pack_start(GTK_BOX(loading_box), loading_lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(loading_revealer), loading_box);

    /* ─── Pages ─── */
    dashboard_page = create_dashboard_page();
    courses_page   = create_courses_page();
    logs_page      = create_logs_page();

    gtk_stack_add_named(GTK_STACK(stack), dashboard_page, "dashboard");
    gtk_stack_add_named(GTK_STACK(stack), courses_page,   "courses");
    gtk_stack_add_named(GTK_STACK(stack), logs_page,      "logs");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "dashboard");

    /* Connect signals */
    g_signal_connect(btn_mandatory,    "clicked", G_CALLBACK(on_mandatory_clicked), NULL);
    g_signal_connect(btn_stress,       "clicked", G_CALLBACK(on_stress_clicked), NULL);
    g_signal_connect(clear_log_button, "clicked", G_CALLBACK(on_clear_log_clicked), NULL);

    gtk_container_add(GTK_CONTAINER(main_window), main_box);
}

/* ==================== MAIN ==================== */
int main(int argc, char *argv[]) {
    srand((unsigned int)time(NULL));
    logFp = fopen("registration_log.txt", "w");
    if (!logFp) fprintf(stderr, "Warning: Cannot create registration_log.txt\n");

    gtk_init(&argc, &argv);

    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), "Course Registration System");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 1300, 820);
    gtk_window_set_position(GTK_WINDOW(main_window), GTK_WIN_POS_CENTER);
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    build_gui();
    gtk_widget_show_all(main_window);

    /* Hide loading overlay initially */
    gtk_revealer_set_reveal_child(GTK_REVEALER(loading_revealer), FALSE);

    gtk_main();

    if (logFp) fclose(logFp);
    for (int i = 0; i < numCourses; i++) pthread_mutex_destroy(&courses[i].seatMutex);
    pthread_mutex_destroy(&statsMutex);
    pthread_mutex_destroy(&logMutex);
    return 0;
}
