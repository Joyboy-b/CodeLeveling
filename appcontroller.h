#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

class AppController : public QObject {
    Q_OBJECT

    Q_PROPERTY(int totalXp READ totalXp NOTIFY totalXpChanged)
    Q_PROPERTY(int level READ level NOTIFY levelChanged)

    Q_PROPERTY(QVariantList quests READ quests NOTIFY questsChanged)
    Q_PROPERTY(QVariantList dailyTasks READ dailyTasks NOTIFY dailyTasksChanged)
    Q_PROPERTY(QVariantList leaderboard READ leaderboard NOTIFY leaderboardChanged)

    Q_PROPERTY(QString currentUser READ currentUser NOTIFY currentUserChanged)
    Q_PROPERTY(QVariantList users READ users NOTIFY usersChanged)

public:
    explicit AppController(QObject *parent = nullptr);

    int totalXp() const { return m_totalXp; }
    int level() const { return m_level; }

    QVariantList quests() const { return m_quests; }
    QVariantList dailyTasks() const { return m_dailyTasks; }
    QVariantList leaderboard() const { return m_leaderboard; }

    QString currentUser() const { return m_currentUser; }
    QVariantList users() const { return m_users; }

    Q_INVOKABLE void refresh();

    Q_INVOKABLE void completeQuest(int questId, int xpEarned, int score);
    Q_INVOKABLE QVariantMap getNextQuestion(int questId);
    Q_INVOKABLE bool submitAnswer(int questionId, const QVariant &userAnswer);
    Q_INVOKABLE QString getLesson(int questId);

    Q_INVOKABLE void completeDailyTask(int taskId);
    Q_INVOKABLE void refreshDaily();
    Q_INVOKABLE void refreshLeaderboard();

    Q_INVOKABLE void setCurrentUser(const QString& username);

signals:
    void totalXpChanged();
    void levelChanged();
    void questsChanged();
    void dailyTasksChanged();
    void leaderboardChanged();

    void currentUserChanged();
    void usersChanged();

    void toast(QString msg);

private:
    void loadStats();
    void loadQuests();
    void loadDailyTasks();
    void loadLeaderboard();
    void loadUsers();

    bool ensureUser(const QString& username);

    int computeLevel(int xp) const;

    int m_totalXp = 0;
    int m_level = 1;

    int m_userId = -1;                 // better default than 1
    QString m_currentUser = "LocalUser";

    QVariantList m_quests;
    QVariantList m_dailyTasks;
    QVariantList m_leaderboard;

    QVariantList m_users;
};

