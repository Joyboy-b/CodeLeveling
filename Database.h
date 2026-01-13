#pragma once
#include <QString>
#include <QSqlDatabase>

class Database {
public:
    static bool init();
    static QSqlDatabase db();
    static QString dbPath();

private:
    static QSqlDatabase s_db;
    static bool createTables();
    static bool seedIfEmpty();
    static bool seedQuestionsIfEmpty();
    static bool seedLessonsIfEmpty();
};
