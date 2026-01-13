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

    if (!createTables()) return false;
    if (!seedIfEmpty()) return false;
    return true;
}

bool Database::createTables() {
    QSqlQuery q(s_db);

    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS user_stats(
            id INTEGER PRIMARY KEY CHECK (id = 1),
            total_xp INTEGER NOT NULL DEFAULT 0,
            level INTEGER NOT NULL DEFAULT 1,
            last_active TEXT
        )
    )")) {
        qWarning() << q.lastError().text();
        return false;
    }

    if (!q.exec(R"(
        INSERT OR IGNORE INTO user_stats(id, total_xp, level, last_active)
        VALUES(1, 0, 1, datetime('now'))
    )")) {
        qWarning() << q.lastError().text();
        return false;
    }

    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS quests(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            topic TEXT NOT NULL,
            difficulty INTEGER NOT NULL DEFAULT 1,
            unlocked INTEGER NOT NULL DEFAULT 1
        )
    )")) {
        qWarning() << q.lastError().text();
        return false;
    }

    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS quest_progress(
            quest_id INTEGER PRIMARY KEY,
            status TEXT NOT NULL DEFAULT 'locked',   -- locked|unlocked|completed
            best_score INTEGER NOT NULL DEFAULT 0,
            last_attempt TEXT
        )
    )")) {
        qWarning() << q.lastError().text();
        return false;
    }

    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS lessons(
            quest_id INTEGER PRIMARY KEY,
            body TEXT NOT NULL
        )
    )")) {
        qWarning() << q.lastError().text();
        return false;
    }


    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS questions(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            quest_id INTEGER NOT NULL,
            type TEXT NOT NULL,              -- mcq (for now)
            prompt TEXT NOT NULL,
            choices_json TEXT NOT NULL,      -- JSON array
            answer_json TEXT NOT NULL,       -- JSON object
            xp_value INTEGER NOT NULL DEFAULT 10
        )
    )")) {
        qWarning() << q.lastError().text();
        return false;
    }

    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS attempts(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            question_id INTEGER NOT NULL,
            timestamp TEXT NOT NULL DEFAULT (datetime('now')),
            is_correct INTEGER NOT NULL,
            user_answer_json TEXT NOT NULL
        )
    )")) {
        qWarning() << q.lastError().text();
        return false;
    }

    if (!q.exec(R"(CREATE INDEX IF NOT EXISTS idx_attempts_qid ON attempts(question_id))")) {
        qWarning() << q.lastError().text();
        return false;
    }

    return true;
}

bool Database::seedIfEmpty() {
    QSqlQuery q(s_db);

    if (!q.exec("SELECT COUNT(*) FROM quests")) {
        qWarning() << q.lastError().text();
        return false;
    }
    if (!q.next()) return false;

    const int count = q.value(0).toInt();
    if (count > 0) {
        if (!seedQuestionsIfEmpty()) return false;
        if (!seedLessonsIfEmpty()) return false;
        return true;
    }


    // Seed a few starter quests
    struct Seed { const char* title; const char* topic; int diff; };
    Seed seeds[] = {
                    {"Arrays I: Basics", "arrays", 1},
                    {"Pointers I: Addresses", "pointers", 2},
                    {"Recursion I: Base Case", "recursion", 2},
                    };

    QSqlQuery ins(s_db);
    ins.prepare("INSERT INTO quests(title, topic, difficulty, unlocked) VALUES(?, ?, ?, 1)");

    for (auto &s : seeds) {
        ins.addBindValue(QString::fromUtf8(s.title));
        ins.addBindValue(QString::fromUtf8(s.topic));
        ins.addBindValue(s.diff);
        if (!ins.exec()) {
            qWarning() << ins.lastError().text();
            return false;
        }
    }

    // Unlock first quest, lock others
    QSqlQuery prog(s_db);
    if (!prog.exec("SELECT id FROM quests ORDER BY id ASC")) return false;

    bool first = true;
    while (prog.next()) {
        int id = prog.value(0).toInt();
        QSqlQuery up(s_db);
        up.prepare("INSERT OR REPLACE INTO quest_progress(quest_id, status) VALUES(?, ?)");
        up.addBindValue(id);
        up.addBindValue(first ? "unlocked" : "locked");
        if (!up.exec()) return false;
        first = false;
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

