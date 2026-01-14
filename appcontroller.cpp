#include "AppController.h"
#include "Database.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>


AppController::AppController(QObject *parent) : QObject(parent) {
    loadUsers();  // populate App.users for ComboBox ASAP

    if (!ensureUser("LocalUser")) {
        qWarning() << "Failed to init default user";
        return;
    }

    loadUsers();  // LocalUser might have just been created
    refresh();    // stats/quests/dailies/leaderboard for this user
}


int AppController::computeLevel(int xp) const {
    // Simple leveling: every 200 XP = +1 level
    return 1 + (xp / 200);
}

void AppController::refresh() {
    loadStats();
    loadQuests();
    loadDailyTasks();
    loadLeaderboard();
}

void AppController::loadStats() {
    QSqlQuery q(Database::db());
    q.prepare("SELECT total_xp, level FROM user_stats WHERE user_id = ?");
    q.addBindValue(m_userId);

    if (!q.exec()) { qWarning() << q.lastError().text(); return; }
    if (q.next()) {
        m_totalXp = q.value(0).toInt();
        m_level = q.value(1).toInt();
        emit totalXpChanged();
        emit levelChanged();
    }
}


void AppController::loadQuests() {
    QVariantList list;

    QSqlQuery q(Database::db());
    q.prepare(R"(
        SELECT q.id, q.title, q.topic, q.difficulty,
               COALESCE(p.status, 'locked') as status,
               COALESCE(p.best_score, 0) as best_score
        FROM quests q
        LEFT JOIN quest_progress p
          ON p.quest_id = q.id AND p.user_id = ?
        ORDER BY q.id ASC
    )");
    q.addBindValue(m_userId);

    if (!q.exec()) {
        qWarning() << q.lastError().text();
        return;
    }

    while (q.next()) {
        QVariantMap m;
        m["id"] = q.value(0).toInt();
        m["title"] = q.value(1).toString();
        m["topic"] = q.value(2).toString();
        m["difficulty"] = q.value(3).toInt();
        m["status"] = q.value(4).toString();
        m["bestScore"] = q.value(5).toInt();
        list.append(m);
    }

    m_quests = list;
    emit questsChanged();
}


void AppController::completeQuest(int questId, int xpEarned, int score) {
    // Mark completed, update best score, unlock next quest
    {
        QSqlQuery q(Database::db());
        q.prepare(R"(
        INSERT INTO quest_progress(user_id, quest_id, status, best_score, last_attempt)
        VALUES(?, ?, 'completed', ?, datetime('now'))
        ON CONFLICT(user_id, quest_id) DO UPDATE SET
            status='completed',
            best_score=MAX(best_score, excluded.best_score),
            last_attempt=datetime('now')
        )");
        q.addBindValue(m_userId);
        q.addBindValue(questId);
        q.addBindValue(score);

        if (!q.exec()) {
            emit toast("DB error: failed to save progress");
            return;
        }
    }

    // Unlock next quest by id order
    int nextId = -1;
    {
        QSqlQuery n(Database::db());
        n.prepare("SELECT id FROM quests WHERE id > ? ORDER BY id ASC LIMIT 1");
        n.addBindValue(questId);
        if (n.exec() && n.next()) nextId = n.value(0).toInt();
    }

    if (nextId > 0) {
        QSqlQuery q(Database::db());
        q.prepare(R"(
        INSERT INTO quest_progress(user_id, quest_id, status)
        VALUES(?, ?, 'unlocked')
        ON CONFLICT(user_id, quest_id) DO UPDATE SET status='unlocked'
    )");
        q.addBindValue(m_userId);
        q.addBindValue(nextId);
        q.exec();
    }


    // Add XP + recompute level
    m_totalXp += xpEarned;
    int newLevel = computeLevel(m_totalXp);

    {
        QSqlQuery q(Database::db());
        q.prepare("UPDATE user_stats SET total_xp=?, level=?, last_active=datetime('now') WHERE user_id=?");
        q.addBindValue(m_totalXp);
        q.addBindValue(newLevel);
        q.addBindValue(m_userId);
        if (!q.exec()) {
            emit toast("DB error: failed to update XP");
            return;
        }
    }

    if (newLevel != m_level) {
        m_level = newLevel;
        emit levelChanged();
        emit toast("Level up!");
    }

    emit totalXpChanged();
    loadQuests();
    emit toast("Quest completed +XP");
}

QVariantMap AppController::getNextQuestion(int questId) {
    QVariantMap out;

    QSqlQuery q(Database::db());
    // Pick first question not yet answered correctly; fallback to first question.
    q.prepare(R"(
        SELECT qu.id, qu.type, qu.prompt, qu.choices_json, qu.answer_json, qu.xp_value
        FROM questions qu
        LEFT JOIN (
            SELECT question_id, MAX(is_correct) AS any_correct
            FROM attempts
            WHERE user_id = ?
            GROUP BY question_id
        ) a ON a.question_id = qu.id
        WHERE qu.quest_id = ?
            AND COALESCE(a.any_correct, 0) = 0
        ORDER BY qu.id ASC
        LIMIT 1
    )");
    q.addBindValue(m_userId);
    q.addBindValue(questId);

    if (!q.exec()) return out;

    if (!q.next()) {
        // No unanswered questions left => quest mastered
        return out; // empty map
    }

    const int id = q.value(0).toInt();
    const QString type = q.value(1).toString();
    const QString prompt = q.value(2).toString();
    const QString choicesStr = q.value(3).toString();
    const int xp = q.value(5).toInt();

    // choices_json -> QVariantList
    QVariantList choices;
    {
        const auto doc = QJsonDocument::fromJson(choicesStr.toUtf8());
        if (doc.isArray()) {
            for (auto v : doc.array()) choices.append(v.toVariant());
        }
    }

    out["id"] = id;
    out["type"] = type;
    out["prompt"] = prompt;
    out["choices"] = choices;
    out["xp"] = xp;
    return out;
}

bool AppController::submitAnswer(int questionId, const QVariant &userAnswer) {
    // Load correct answer
    QSqlQuery q(Database::db());
    q.prepare("SELECT quest_id, answer_json, xp_value FROM questions WHERE id = ?");
    q.addBindValue(questionId);
    if (!q.exec() || !q.next()) {
        emit toast("Question not found");
        return false;
    }

    const int questId = q.value(0).toInt();
    const QString answerStr = q.value(1).toString();
    const int xpValue = q.value(2).toInt();

    int correctIndex = -1;
    {
        const auto doc = QJsonDocument::fromJson(answerStr.toUtf8());
        if (doc.isObject()) correctIndex = doc.object().value("correctIndex").toInt(-1);
    }

    const int userIndex = userAnswer.toInt(); // for MCQ we pass index
    const bool correct = (userIndex == correctIndex);
    bool alreadyCorrect = false;
    if (correct) {
        QSqlQuery prev(Database::db());
        prev.prepare("SELECT 1 FROM attempts WHERE user_id=? AND question_id=? AND is_correct=1 LIMIT 1");
        prev.addBindValue(m_userId);
        prev.addBindValue(questionId);
        if (prev.exec() && prev.next()) alreadyCorrect = true;
    }
    // Save attempt
    {
        QJsonObject ua;
        ua["selectedIndex"] = userIndex;
        const QString uaStr = QString::fromUtf8(QJsonDocument(ua).toJson(QJsonDocument::Compact));

        QSqlQuery ins(Database::db());
        ins.prepare(R"(
            INSERT INTO attempts(user_id, question_id, is_correct, user_answer_json)
            VALUES(?, ?, ?, ?)
        )");
        ins.addBindValue(m_userId);
        ins.addBindValue(questionId);
        ins.addBindValue(correct ? 1 : 0);
        ins.addBindValue(uaStr);

        if (!ins.exec()) {
            emit toast("DB error saving attempt");
            return false;
        }
    }

    if (correct) {
        if (alreadyCorrect) {
            emit toast("Correct (already mastered). No XP awarded.");
        } else {
            m_totalXp += xpValue;
            const int newLevel = computeLevel(m_totalXp);

            QSqlQuery upd(Database::db());
            upd.prepare("UPDATE user_stats SET total_xp=?, level=?, last_active=datetime('now') WHERE user_id=?");
            upd.addBindValue(m_totalXp);
            upd.addBindValue(newLevel);
            upd.addBindValue(m_userId);
            if (upd.exec()) {
                emit totalXpChanged();
                if (newLevel != m_level) {
                    m_level = newLevel;
                    emit levelChanged();
                    emit toast("Correct! Level up!");
                } else {
                    emit toast(QString("Correct! +%1 XP").arg(xpValue));
                }
            }
        }

        // If all questions in quest have at least one correct attempt, complete quest (0 XP here)
        QSqlQuery qc(Database::db());
        qc.prepare(R"(
            SELECT
            (SELECT COUNT(*) FROM questions WHERE quest_id = ?) AS total_q,
            (SELECT COUNT(DISTINCT qu.id)
              FROM questions qu
              JOIN attempts a
                ON a.question_id = qu.id
               AND a.is_correct = 1
               AND a.user_id = ?
              WHERE qu.quest_id = ?) AS correct_q
        )");
        qc.addBindValue(questId);
        qc.addBindValue(m_userId);
        qc.addBindValue(questId);

        if (qc.exec() && qc.next()) {
            const int totalQ = qc.value(0).toInt();
            const int correctQ = qc.value(1).toInt();
            if (totalQ > 0 && correctQ >= totalQ) {
                completeQuest(questId, 0, 100);
            }
        }

        loadQuests();
        return true;
    } else {
        emit toast("Not quite. Try again.");
        return false;
    }
}

QString AppController::getLesson(int questId) {
    QSqlQuery q(Database::db());
    q.prepare("SELECT body FROM lessons WHERE quest_id = ?");
    q.addBindValue(questId);
    if (q.exec() && q.next()) return q.value(0).toString();
    return "";
}

bool AppController::ensureUser(const QString& username) {
    QSqlQuery ins(Database::db());
    ins.prepare("INSERT OR IGNORE INTO users(username) VALUES(?)");
    ins.addBindValue(username);
    if (!ins.exec()) return false;

    QSqlQuery q(Database::db());
    q.prepare("SELECT id FROM users WHERE username=? LIMIT 1");
    q.addBindValue(username);
    if (!q.exec() || !q.next()) return false;

    m_userId = q.value(0).toInt();
    m_currentUser = username;

    if (!Database::initProgressForUser(m_userId)) {
        qWarning() << "Failed to init progress for user" << m_userId;
        // you can return false here if you want to fail hard:
        // return false;
        return false;
    }

    QSqlQuery st(Database::db());
    st.prepare("INSERT OR IGNORE INTO user_stats(user_id,total_xp,level,last_active) VALUES(?,0,1,datetime('now'))");
    st.addBindValue(m_userId);
    st.exec();

    emit currentUserChanged();
    return true;
}


void AppController::setCurrentUser(const QString& username) {
    const QString u = username.trimmed();
    if (u.isEmpty()) return;

    if (!ensureUser(u)) {
        emit toast("Failed to switch user");
        return;
    }

    loadUsers(); // update ComboBox if new user was added
    refresh();   // loads stats/quests/dailies/leaderboard
    emit toast("Switched user: " + m_currentUser);
}

void AppController::loadDailyTasks() {
    QVariantList out;

    QSqlQuery q(Database::db());
    q.prepare(R"(
        SELECT dt.id, dt.title, dt.xp_value,
               EXISTS(
                 SELECT 1 FROM daily_completions dc
                 WHERE dc.user_id = ?
                   AND dc.task_id = dt.id
                   AND dc.day = date('now')
               ) AS done_today
        FROM daily_tasks dt
        WHERE dt.active = 1
        ORDER BY dt.id ASC
    )");
    q.addBindValue(m_userId);

    if (!q.exec()) return;

    while (q.next()) {
        QVariantMap m;
        m["id"] = q.value(0).toInt();
        m["title"] = q.value(1).toString();
        m["xp"] = q.value(2).toInt();
        m["done"] = (q.value(3).toInt() == 1);
        out.append(m);
    }

    m_dailyTasks = out;
    emit dailyTasksChanged();
}

void AppController::refreshDaily() { loadDailyTasks(); }

void AppController::completeDailyTask(int taskId) {
    // already done today?
    QSqlQuery chk(Database::db());
    chk.prepare("SELECT 1 FROM daily_completions WHERE user_id=? AND task_id=? AND day=date('now') LIMIT 1");
    chk.addBindValue(m_userId);
    chk.addBindValue(taskId);
    if (chk.exec() && chk.next()) {
        emit toast("Daily already completed today.");
        return;
    }

    // get XP
    QSqlQuery q(Database::db());
    q.prepare("SELECT xp_value FROM daily_tasks WHERE id=? AND active=1");
    q.addBindValue(taskId);
    if (!q.exec() || !q.next()) { emit toast("Daily task not found"); return; }
    int xp = q.value(0).toInt();

    // insert completion
    QSqlQuery ins(Database::db());
    ins.prepare("INSERT INTO daily_completions(user_id, task_id, day) VALUES(?, ?, date('now'))");
    ins.addBindValue(m_userId);
    ins.addBindValue(taskId);
    if (!ins.exec()) { emit toast("Failed to save daily completion"); return; }

    // add XP
    m_totalXp += xp;
    int newLevel = computeLevel(m_totalXp);

    QSqlQuery upd(Database::db());
    upd.prepare("UPDATE user_stats SET total_xp=?, level=?, last_active=datetime('now') WHERE user_id=?");
    upd.addBindValue(m_totalXp);
    upd.addBindValue(newLevel);
    upd.addBindValue(m_userId);
    upd.exec();

    emit totalXpChanged();
    if (newLevel != m_level) { m_level = newLevel; emit levelChanged(); emit toast("Daily complete + Level up!"); }
    else emit toast(QString("Daily complete +%1 XP").arg(xp));

    loadDailyTasks();
    loadLeaderboard();
}
void AppController::loadLeaderboard() {
    QVariantList out;

    QSqlQuery q(Database::db());
    q.exec(R"(
        SELECT u.username,
               s.total_xp,
               s.level,
               s.last_active,
               (s.total_xp +
                 MAX(0, 200 - (julianday('now') - julianday(s.last_active)) * 20)
               ) AS rank_score
        FROM user_stats s
        JOIN users u ON u.id = s.user_id
        ORDER BY rank_score DESC
        LIMIT 20
    )");

    while (q.next()) {
        QVariantMap m;
        m["username"] = q.value(0).toString();
        m["xp"] = q.value(1).toInt();
        m["level"] = q.value(2).toInt();
        m["lastActive"] = q.value(3).toString();
        m["score"] = q.value(4).toDouble();
        out.append(m);
    }

    m_leaderboard = out;
    emit leaderboardChanged();
}

void AppController::refreshLeaderboard() { loadLeaderboard(); }

void AppController::loadUsers() {
    QVariantList out;

    QSqlQuery q(Database::db());
    if (!q.exec("SELECT username FROM users ORDER BY username COLLATE NOCASE ASC")) {
        qWarning() << q.lastError().text();
        return;
    }

    while (q.next()) out.append(q.value(0).toString());

    m_users = out;
    emit usersChanged();
}



