#pragma once
#include <QObject>
#include <QVariantList>

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(int totalXp READ totalXp NOTIFY totalXpChanged)
    Q_PROPERTY(int level READ level NOTIFY levelChanged)
    Q_PROPERTY(QVariantList quests READ quests NOTIFY questsChanged)

public:
    explicit AppController(QObject *parent = nullptr);

    int totalXp() const { return m_totalXp; }
    int level() const { return m_level; }
    QVariantList quests() const { return m_quests; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void completeQuest(int questId, int xpEarned, int score);
    Q_INVOKABLE QVariantMap getNextQuestion(int questId);
    Q_INVOKABLE bool submitAnswer(int questionId, const QVariant &userAnswer);
    Q_INVOKABLE QString getLesson(int questId);


signals:
    void totalXpChanged();
    void levelChanged();
    void questsChanged();
    void toast(QString msg);

private:
    void loadStats();
    void loadQuests();
    int computeLevel(int xp) const; // simple leveling

    int m_totalXp = 0;
    int m_level = 1;
    QVariantList m_quests;
};
