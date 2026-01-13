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
    refresh();
}

int AppController::computeLevel(int xp) const {
    // Simple leveling: every 200 XP = +1 level
    return 1 + (xp / 200);
}

void AppController::refresh() {
    loadStats();
    loadQuests();
}

void AppController::loadStats() {
    QSqlQuery q(Database::db());
    if (!q.exec("SELECT total_xp, level FROM user_stats WHERE id = 1")) {
        qWarning() << q.lastError().text();
        return;
    }
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
    if (!q.exec(R"(
        SELECT q.id, q.title, q.topic, q.difficulty,
               COALESCE(p.status, 'locked') as status,
               COALESCE(p.best_score, 0) as best_score
        FROM quests q
        LEFT JOIN quest_progress p ON p.quest_id = q.id
        ORDER BY q.id ASC
    )")) {
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
            INSERT INTO quest_progress(quest_id, status, best_score, last_attempt)
            VALUES(?, 'completed', ?, datetime('now'))
            ON CONFLICT(quest_id) DO UPDATE SET
                status='completed',
                best_score=MAX(best_score, excluded.best_score),
                last_attempt=datetime('now')
        )");
        q.addBindValue(questId);
        q.addBindValue(score);
        if (!q.exec()) {
            emit toast("DB error: failed to save progress");
            return;
        }
    }

    // Unlock next quest by id order
    {
        QSqlQuery q(Database::db());
        q.prepare(R"(
            UPDATE quest_progress
            SET status='unlocked'
            WHERE quest_id = (
                SELECT id FROM quests WHERE id > ? ORDER BY id ASC LIMIT 1
            )
            AND status='locked'
        )");
        q.addBindValue(questId);
        q.exec(); // if none exists, fine
    }

    // Add XP + recompute level
    m_totalXp += xpEarned;
    int newLevel = computeLevel(m_totalXp);

    {
        QSqlQuery q(Database::db());
        q.prepare("UPDATE user_stats SET total_xp=?, level=?, last_active=datetime('now') WHERE id=1");
        q.addBindValue(m_totalXp);
        q.addBindValue(newLevel);
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
            GROUP BY question_id
        ) a ON a.question_id = qu.id
        WHERE qu.quest_id = ?
          AND COALESCE(a.any_correct, 0) = 0
        ORDER BY qu.id ASC
        LIMIT 1
    )");
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
        prev.prepare("SELECT 1 FROM attempts WHERE question_id = ? AND is_correct = 1 LIMIT 1");
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
            INSERT INTO attempts(question_id, is_correct, user_answer_json)
            VALUES(?, ?, ?)
        )");
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
            upd.prepare("UPDATE user_stats SET total_xp=?, level=?, last_active=datetime('now') WHERE id=1");
            upd.addBindValue(m_totalXp);
            upd.addBindValue(newLevel);
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
               JOIN attempts a ON a.question_id = qu.id AND a.is_correct = 1
               WHERE qu.quest_id = ?) AS correct_q
        )");
        qc.addBindValue(questId);
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


