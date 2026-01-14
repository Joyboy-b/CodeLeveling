#include "database.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

QSqlDatabase Database::s_db;

QString Database::dbPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/codeleveling.sqlite";
}

QSqlDatabase Database::db() { return s_db; }

bool Database::init() {
    if (QSqlDatabase::contains("codeleveling"))
        s_db = QSqlDatabase::database("codeleveling");
    else
        s_db = QSqlDatabase::addDatabase("QSQLITE", "codeleveling");

    s_db.setDatabaseName(dbPath());

    if (!s_db.open()) {
        qWarning() << "DB open failed:" << s_db.lastError().text();
        return false;
    }

    // IMPORTANT: do this after open()
    {
        QSqlQuery pragma(s_db);
        if (!pragma.exec("PRAGMA foreign_keys = ON;")) {
            qWarning() << "Failed to enable foreign keys:" << pragma.lastError().text();
        }
    }

    if (!createTables()) return false;
    if (!seedIfEmpty()) return false;
    if (!seedDailyTasksIfEmpty()) return false;

    int uid = ensureDefaultUser();
    if (uid <= 0) return false;

    if (!Database::initProgressForUser(uid)) return false;


    return true;
}


bool Database::createTables() {
    QSqlQuery q(s_db);

    // ---- Users ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS users(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL UNIQUE,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    // ---- Per-user stats ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS user_stats(
            user_id INTEGER PRIMARY KEY,
            total_xp INTEGER NOT NULL DEFAULT 0,
            level INTEGER NOT NULL DEFAULT 1,
            last_active TEXT,
            FOREIGN KEY(user_id) REFERENCES users(id)
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    // ---- Quests (global definitions) ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS quests(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            topic TEXT NOT NULL,
            difficulty INTEGER NOT NULL DEFAULT 1
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    // ---- Quest progress (per user) ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS quest_progress(
            user_id INTEGER NOT NULL,
            quest_id INTEGER NOT NULL,
            status TEXT NOT NULL DEFAULT 'locked',   -- locked|unlocked|completed
            best_score INTEGER NOT NULL DEFAULT 0,
            last_attempt TEXT,
            PRIMARY KEY(user_id, quest_id),
            FOREIGN KEY(user_id) REFERENCES users(id),
            FOREIGN KEY(quest_id) REFERENCES quests(id)
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    // ---- Lessons (global, per quest) ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS lessons(
            quest_id INTEGER PRIMARY KEY,
            body TEXT NOT NULL,
            FOREIGN KEY(quest_id) REFERENCES quests(id)
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    // ---- Questions (global, per quest) ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS questions(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            quest_id INTEGER NOT NULL,
            type TEXT NOT NULL,              -- mcq (for now)
            prompt TEXT NOT NULL,
            choices_json TEXT NOT NULL,
            answer_json TEXT NOT NULL,
            xp_value INTEGER NOT NULL DEFAULT 10,
            FOREIGN KEY(quest_id) REFERENCES quests(id)
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    // ---- Attempts (per user) ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS attempts(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER NOT NULL,
            question_id INTEGER NOT NULL,
            timestamp TEXT NOT NULL DEFAULT (datetime('now')),
            is_correct INTEGER NOT NULL,
            user_answer_json TEXT NOT NULL,
            FOREIGN KEY(user_id) REFERENCES users(id),
            FOREIGN KEY(question_id) REFERENCES questions(id)
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    if (!q.exec(R"(CREATE INDEX IF NOT EXISTS idx_attempts_user_qid ON attempts(user_id, question_id))")) {
        qWarning() << q.lastError().text();
        return false;
    }

    // ---- Daily tasks (global definitions) ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS daily_tasks(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            xp_value INTEGER NOT NULL DEFAULT 10,
            active INTEGER NOT NULL DEFAULT 1
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    // ---- Daily completions (per user, per day) ----
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS daily_completions(
            user_id INTEGER NOT NULL,
            task_id INTEGER NOT NULL,
            day TEXT NOT NULL,
            completed_at TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY(user_id, task_id, day),
            FOREIGN KEY(user_id) REFERENCES users(id),
            FOREIGN KEY(task_id) REFERENCES daily_tasks(id)
        )
    )")) { qWarning() << q.lastError().text(); return false; }

    q.exec("CREATE INDEX IF NOT EXISTS idx_daily_day ON daily_completions(day)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_daily_user ON daily_completions(user_id)");

    return true;
}


bool Database::seedIfEmpty() {
    QSqlQuery q(s_db);

    if (!q.exec("SELECT COUNT(*) FROM quests")) return false;
    if (!q.next()) return false;

    const int count = q.value(0).toInt();
    if (count > 0) {
        if (!seedQuestionsIfEmpty()) return false;
        if (!seedLessonsIfEmpty()) return false;
        return true;
    }

    struct Seed { const char* title; const char* topic; int diff; };
    Seed seeds[] = {
                    {"Arrays I: Basics", "arrays", 1},
                    {"Pointers I: Addresses", "pointers", 2},
                    {"Recursion I: Base Case", "recursion", 2},
                    };

    QSqlQuery ins(s_db);
    ins.prepare("INSERT INTO quests(title, topic, difficulty) VALUES(?, ?, ?)");

    for (auto &s : seeds) {
        ins.addBindValue(QString::fromUtf8(s.title));
        ins.addBindValue(QString::fromUtf8(s.topic));
        ins.addBindValue(s.diff);
        if (!ins.exec()) {
            qWarning() << ins.lastError().text();
            return false;
        }
    }

    if (!seedQuestionsIfEmpty()) return false;
    if (!seedLessonsIfEmpty()) return false;

    return true;
}


bool Database::seedQuestionsIfEmpty() {
    QSqlQuery q(s_db);

    if (!q.exec("SELECT COUNT(*) FROM questions")) {
        qWarning() << q.lastError().text();
        return false;
    }
    if (!q.next()) return false;

    const int count = q.value(0).toInt();
    if (count > 0) return true;

    // Get quest IDs by topic
    QSqlQuery qs(s_db);
    if (!qs.exec("SELECT id, topic FROM quests")) {
        qWarning() << qs.lastError().text();
        return false;
    }

    int arraysId = -1, pointersId = -1, recursionId = -1;
    while (qs.next()) {
        const int id = qs.value(0).toInt();
        const QString topic = qs.value(1).toString();
        if (topic == "arrays") arraysId = id;
        else if (topic == "pointers") pointersId = id;
        else if (topic == "recursion") recursionId = id;
    }

    auto choicesJson = [](std::initializer_list<const char*> items) {
        QJsonArray arr;
        for (auto s : items) arr.append(QString::fromUtf8(s));
        return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    };

    auto answerJsonIndex = [](int correctIndex) {
        QJsonObject obj;
        obj["correctIndex"] = correctIndex;
        return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    };

    QSqlQuery ins(s_db);
    ins.prepare(R"(
        INSERT INTO questions(quest_id, type, prompt, choices_json, answer_json, xp_value)
        VALUES(?, 'mcq', ?, ?, ?, ?)
    )");

    auto addQ = [&](int questId, const QString& prompt,
                    const QString& choices, const QString& answer, int xp) {
        if (questId <= 0) return true; // quest missing? skip safely
        ins.addBindValue(questId);
        ins.addBindValue(prompt);
        ins.addBindValue(choices);
        ins.addBindValue(answer);
        ins.addBindValue(xp);
        if (!ins.exec()) {
            qWarning() << ins.lastError().text();
            return false;
        }
        return true;
    };

    // Arrays questions
    if (!addQ(arraysId,
              "What is the index of the first element in a C++ array?",
              choicesJson({"0", "1", "Depends on array size", "-1"}),
              answerJsonIndex(0),
              20)) return false;

    if (!addQ(arraysId,
              "If int a[5]; what is the last valid index?",
              choicesJson({"5", "4", "3", "1"}),
              answerJsonIndex(1),
              25)) return false;

    // Pointers questions
    if (!addQ(pointersId,
              "What does the operator '&' usually mean in 'int* p = &x;' ?",
              choicesJson({"Address-of", "Dereference", "Bitwise NOT", "Modulo"}),
              answerJsonIndex(0),
              25)) return false;

    if (!addQ(pointersId,
              "If p is an int*, what does *p represent?",
              choicesJson({"The pointer address", "The value pointed to", "A reference type", "An array"}),
              answerJsonIndex(1),
              25)) return false;

    // Recursion questions
    if (!addQ(recursionId,
              "In recursion, what is the purpose of the base case?",
              choicesJson({"Make it faster", "Stop infinite recursion", "Use loops", "Allocate memory"}),
              answerJsonIndex(1),
              30)) return false;

    if (!addQ(recursionId,
              "Which is most likely recursive? (Pick the best answer)",
              choicesJson({"Printing 1..n using a loop", "Binary search implementation", "Sorting by swapping neighbors once", "Assigning variables"}),
              answerJsonIndex(1),
              30)) return false;

    return true;
}

bool Database::seedLessonsIfEmpty() {
    QSqlQuery q(s_db);
    if (!q.exec("SELECT COUNT(*) FROM lessons")) return false;
    if (!q.next()) return false;

    const int count = q.value(0).toInt();
    if (count > 0) return true;

    // Map quest_id by topic
    QSqlQuery qs(s_db);
    if (!qs.exec("SELECT id, topic FROM quests")) return false;

    int arraysId = -1, pointersId = -1, recursionId = -1;
    while (qs.next()) {
        const int id = qs.value(0).toInt();
        const QString topic = qs.value(1).toString();
        if (topic == "arrays") arraysId = id;
        else if (topic == "pointers") pointersId = id;
        else if (topic == "recursion") recursionId = id;
    }

    QSqlQuery ins(s_db);
    ins.prepare("INSERT OR REPLACE INTO lessons(quest_id, body) VALUES(?, ?)");

    auto addL = [&](int questId, const QString& body) {
        if (questId <= 0) return true;
        ins.addBindValue(questId);
        ins.addBindValue(body);
        if (!ins.exec()) {
            qWarning() << ins.lastError().text();
            return false;
        }
        return true;
    };

    if (!addL(arraysId,
              "### Arrays (C++)\n"
              "- Arrays store elements contiguously in memory.\n"
              "- Indexing starts at **0**.\n"
              "- If `int a[5];` valid indices are `0..4`.\n"
              "- Access: `a[i]`.\n")) return false;

    if (!addL(pointersId,
              "### Pointers (C++)\n"
              "- `&x` means **address of x**.\n"
              "- `int* p = &x;` stores x’s address in p.\n"
              "- `*p` means **the value at that address** (dereference).\n")) return false;

    if (!addL(recursionId,
              "### Recursion\n"
              "- A recursive function calls itself on a smaller problem.\n"
              "- The **base case** stops recursion.\n"
              "- Without a base case, you usually get infinite recursion.\n")) return false;

    return true;
}

int Database::ensureDefaultUser() {
    QSqlQuery ins(s_db);
    ins.exec("INSERT OR IGNORE INTO users(username) VALUES('LocalUser')");

    QSqlQuery q(s_db);
    q.exec("SELECT id FROM users WHERE username='LocalUser' LIMIT 1");
    if (!q.next()) return -1;
    int uid = q.value(0).toInt();

    QSqlQuery st(s_db);
    st.prepare("INSERT OR IGNORE INTO user_stats(user_id,total_xp,level,last_active) VALUES(?,0,1,datetime('now'))");
    st.addBindValue(uid);
    st.exec();

    return uid;
}
bool Database::seedDailyTasksIfEmpty() {
    QSqlQuery q(s_db);
    if (!q.exec("SELECT COUNT(*) FROM daily_tasks")) return false;
    q.next();
    if (q.value(0).toInt() > 0) return true;

    QSqlQuery ins(s_db);
    ins.prepare("INSERT INTO daily_tasks(title, xp_value, active) VALUES(?, ?, 1)");

    auto add = [&](const QString& title, int xp){
        ins.addBindValue(title);
        ins.addBindValue(xp);
        return ins.exec();
    };

    if (!add("Answer 1 quiz question", 15)) return false;
    if (!add("Complete 1 quest attempt", 20)) return false;
    if (!add("Review a lesson", 10)) return false;

    return true;
}

bool Database::initProgressForUser(int userId) {
    QSqlDatabase db = Database::db();

    QSqlQuery q(db);

    // 1) Ensure a row exists for every quest (handles new quests added later)
    q.prepare(R"(
        INSERT OR IGNORE INTO quest_progress(user_id, quest_id, status)
        SELECT ?, id, 'locked'
        FROM quests
    )");
    q.addBindValue(userId);
    if (!q.exec()) {
        qWarning() << "initProgressForUser insert failed:" << q.lastError().text();
        return false;
    }

    // 2) If user has never progressed anything, unlock the first quest
    QSqlQuery chk(db);
    chk.prepare("SELECT 1 FROM quest_progress WHERE user_id=? AND status!='locked' LIMIT 1");
    chk.addBindValue(userId);
    if (!chk.exec()) {
        qWarning() << "initProgressForUser check failed:" << chk.lastError().text();
        return false;
    }

    const bool hasAnyProgress = chk.next();
    if (!hasAnyProgress) {
        QSqlQuery up(db);
        up.prepare(R"(
            UPDATE quest_progress
            SET status='unlocked'
            WHERE user_id=?
              AND quest_id=(SELECT MIN(id) FROM quests)
        )");
        up.addBindValue(userId);
        if (!up.exec()) {
            qWarning() << "initProgressForUser unlock failed:" << up.lastError().text();
            return false;
        }
    }

    return true;
}


