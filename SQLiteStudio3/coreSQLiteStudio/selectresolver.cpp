#include "selectresolver.h"
#include "parser/token.h"
#include "parser/lexer.h"
#include "parser/keywords.h"
#include "schemaresolver.h"
#include "parser/ast/sqlitecreateview.h"
#include "common/global.h"
#include <QDebug>
#include <QHash>
#include <QHashIterator>
#include <QString>

SelectResolver::SelectResolver(Db *db, const QString& originalQuery)
{
    this->db = db;
    this->query = originalQuery;
    schemaResolver = new SchemaResolver(db);
}

SelectResolver::SelectResolver(Db* db, const QString& originalQuery, const BiStrHash& dbNameToAttach) :
    SelectResolver(db, originalQuery)
{
    this->dbNameToAttach = dbNameToAttach;
}

SelectResolver::~SelectResolver()
{
    safe_delete(schemaResolver);
}

QList<SelectResolver::Column> SelectResolver::resolveColumnsFromFirstCore()
{
    if (!parseOriginalQuery())
        return QList<SelectResolver::Column>();

    return resolve(originalQueryParsed->coreSelects.first());
}

QList<QList<SelectResolver::Column>> SelectResolver::resolveColumns()
{
    if (!parseOriginalQuery())
        return QList<QList<SelectResolver::Column>>();

    return resolve(originalQueryParsed.data());
}

QList<SelectResolver::Column> SelectResolver::resolve(SqliteSelect::Core *selectCore)
{
    errors.clear();
    extractCte(selectCore);
    return resolveCore(selectCore);
}

QList<QList<SelectResolver::Column>> SelectResolver::resolve(SqliteSelect *select)
{
    errors.clear();
    extractCte(select);
    QList<QList<SelectResolver::Column> > results;
    for (SqliteSelect::Core* core : select->coreSelects)
    {
        results << resolveCore(core);
        currentCoreResults.clear();
    }

    return results;
}

QList<SelectResolver::Column> SelectResolver::resolveAvailableColumns(SqliteSelect::Core *selectCore)
{
    errors.clear();
    extractCte(selectCore);
    return resolveAvailableCoreColumns(selectCore);
}

QList<QList<SelectResolver::Column> > SelectResolver::resolveAvailableColumns(SqliteSelect *select)
{
    errors.clear();
    extractCte(select);
    QList<QList<SelectResolver::Column> > results;
    for (SqliteSelect::Core* core : select->coreSelects)
        results << resolveAvailableCoreColumns(core);

    return results;
}

QSet<SelectResolver::Table> SelectResolver::resolveTables(SqliteSelect::Core *selectCore)
{
    QSet<Table> tables;
    extractCte(selectCore);
    QList<Column> columns = resolveAvailableColumns(selectCore);
    for (const Column& col : columns)
    {
        if (col.type != Column::Type::COLUMN)
            continue;

        tables << col.getTable();
    }

    return tables;
}

QList<QSet<SelectResolver::Table> > SelectResolver::resolveTables(SqliteSelect *select)
{
    QList<QSet<Table> > results;
    extractCte(select);
    QList<QList<Column> > columnLists = resolveAvailableColumns(select);
    for (const QList<Column>& columns : columnLists)
    {
        QSet<Table> tables;
        for (const Column& col : columns)
        {
            if (col.type != Column::Type::COLUMN)
                continue;

            tables << col.getTable();
        }

        results << tables;
    }

    return results;
}

QList<SelectResolver::Column> SelectResolver::translateToColumns(SqliteSelect* select, const TokenList& columnTokens)
{
    errors.clear();
    extractCte(select);
    QList<SelectResolver::Column> results;
    for (const TokenPtr& token : columnTokens)
        results << translateTokenToColumn(select, token);

    return results;
}

SelectResolver::Column SelectResolver::translateToColumns(SqliteSelect* select, TokenPtr token)
{
    errors.clear();
    return translateTokenToColumn(select, token);
}

bool SelectResolver::hasErrors() const
{
    return !errors.isEmpty();
}

const QStringList& SelectResolver::getErrors() const
{
    return errors;
}

QList<SelectResolver::Column> SelectResolver::resolveCore(SqliteSelect::Core* selectCore)
{
    if (selectCore->from)
        currentCoreSourceColumns = resolveJoinSource(selectCore->from);

    for (SqliteSelect::Core::ResultColumn* resCol : selectCore->resultColumns)
        resolve(resCol);

    if (selectCore->distinctKw)
        markDistinctColumns();

    if (selectCore->groupBy.size() > 0)
        markGroupedColumns();

    fixColumnNames();

    SqliteSelect* select = dynamic_cast<SqliteSelect*>(selectCore->parentStatement());
    if (select && select->coreSelects.size() > 1)
        markCompoundColumns();

    return currentCoreResults;
}

QList<SelectResolver::Column> SelectResolver::resolveAvailableCoreColumns(SqliteSelect::Core* selectCore)
{
    QList<Column> columns;
    if (selectCore->from)
        columns = resolveJoinSource(selectCore->from);

    return columns;
}

SelectResolver::Column SelectResolver::translateTokenToColumn(SqliteSelect* select, TokenPtr token)
{
    QString strippedColName = stripObjName(token->value);

    // Default result
    Column notTranslatedColumn;
    notTranslatedColumn.type = Column::OTHER;
    notTranslatedColumn.column = strippedColName;

    // Find containing statement
    SqliteStatement* parentStmt = select->findStatementWithToken(token);
    if (!parentStmt)
    {
        qDebug() << "Could not find containing statement for given token while translating column token:" << token->toString()
                 << "Select tokens:" << select->tokens.toString();

        return notTranslatedColumn;
    }

    // Go through all select cores, from the most deep, to the most shallow
    SqliteSelect::Core* core = nullptr;
    while (parentStmt)
    {
        // Find nearest SELECT core.
        while (parentStmt && !(core = dynamic_cast<SqliteSelect::Core*>(parentStmt)))
            parentStmt = parentStmt->parentStatement();

        if (!core)
        {
            qDebug() << "Could not find SqliteSelect::Core object for given token while translating column token:" << token->toString()
                     << "Select:" << select->detokenize();

            return notTranslatedColumn;
        }

        // Search through available columns
        for (Column& availableColumn : resolveAvailableColumns(core))
        {
            if (availableColumn.type == Column::COLUMN && availableColumn.column.compare(strippedColName, Qt::CaseInsensitive) == 0)
                return availableColumn;
        }

        // Not in this core. See if there is any core upper (if this was a subselect).
        parentStmt = parentStmt->parentStatement();
    }

    return notTranslatedColumn;
}

void SelectResolver::markDistinctColumns()
{
    markCurrentColumnsWithFlag(FROM_DISTINCT_SELECT);
}

void SelectResolver::markCompoundColumns()
{
    markCurrentColumnsWithFlag(FROM_COMPOUND_SELECT);
}

void SelectResolver::markGroupedColumns()
{
    markCurrentColumnsWithFlag(FROM_GROUPED_SELECT);
}

void SelectResolver::fixColumnNames()
{
    QSet<QString> existingDisplayNames;
    QSet<QString> existingAliasNames;
    QString originalName;
    QString originalAlias;
    QString alias;
    int i;

    QMutableListIterator<Column> it(currentCoreResults);
    while (it.hasNext())
    {
        // Display name
        originalName = it.next().displayName;
        for (i = 1; existingDisplayNames.contains(it.value().displayName); i++)
            it.value().displayName = originalName + ":" + QString::number(i);

        existingDisplayNames << it.value().displayName;

        // Alias
        // Handled both alias duplicates and name duplicates.
        // If name is duplicated, also create alias for it.
        // This is important, because in case of duplicated name/alias, the result column is actually
        // made unique with sequenced number - not only for display, but also for data origin.
        alias = it.value().alias.isNull() ? it.value().column : it.value().alias;
        originalAlias = alias;
        for (i = 1; existingAliasNames.contains(alias); i++)
            alias = originalAlias + ":" + QString::number(i);

        if (alias != originalAlias)
            it.value().alias = alias;

        existingAliasNames << alias;
    }
}

void SelectResolver::markCurrentColumnsWithFlag(SelectResolver::Flag flag, QList<Column>* columnList)
{
    QMutableListIterator<Column> it(columnList ? *columnList : currentCoreResults);
    while (it.hasNext())
        it.next().flags |= flag;
}

void SelectResolver::resolve(SqliteSelect::Core::ResultColumn *resCol)
{
    if (resCol->star)
        resolveStar(resCol);
    else
        resolveExpr(resCol);
}

void SelectResolver::resolveStar(SqliteSelect::Core::ResultColumn *resCol)
{
    bool foundAtLeastOne = false;
    for (SelectResolver::Column column : currentCoreSourceColumns)
    {
        if (!resCol->table.isNull())
        {
            /*
             * Star was prefixed with table or table alias.
             * The "FROM" clause allows to use alias name the same as
             * some other table real name in the very same "FROM".
             * Their columns concatenate, so here we allow any column that
             * prefix matches either alias or table from data source list.
             * For example it's correct to query:
             *     SELECT test.* FROM test, otherTable AS test;
             * This case is simpler then in resolveDbAndTable(),
             * because here's no database allowed.
             *
             * Also, if the table has an alias specified,
             * then the alias has a precedence before table's name,
             * therefore we match table name only if the table alias
             * is null.
             */
            if (
                    (
                        !column.tableAlias.isNull() &&
                        resCol->table.compare(column.tableAlias, Qt::CaseInsensitive) != 0
                    ) ||
                    (
                        column.tableAlias.isNull() &&
                        resCol->table.compare(column.table, Qt::CaseInsensitive) != 0
                    )

                )
            {
                continue;
            }
        }

        // If source column name is aliased, use it
        if (!column.alias.isNull())
            column.displayName = column.alias;
        else
            column.displayName = column.column;

        column.originalColumn = resCol;
        currentCoreResults << column;
        foundAtLeastOne = true;
    }

    if (!foundAtLeastOne)
        errors << QObject::tr("Could not resolve data source for column: %1").arg(resCol->detokenize());
}

void SelectResolver::resolveExpr(SqliteSelect::Core::ResultColumn *resCol)
{
    SqliteExpr* expr = resCol->expr;
    if (expr->mode != SqliteExpr::Mode::ID)
    {
        // Not a simple column, but some expression
        SelectResolver::Column column;
        column.alias = resCol->alias;
        column.originalColumn = resCol;
        column.column = getResColTokensWithoutAlias(resCol).detokenize().trimmed();
        column.displayName = !resCol->alias.isNull() ? column.alias : column.column;

        column.type = Column::OTHER;
        currentCoreResults << column;

        // In this case we end it here.
        return;
    }

    // Now we know we're dealing with db.table.column (with db and table optional)
    resolveDbAndTable(resCol);
}

void SelectResolver::resolveDbAndTable(SqliteSelect::Core::ResultColumn *resCol)
{
    SqliteExpr* expr = resCol->expr;

    // Basic info
    Column col;
    col.alias = resCol->alias;
    col.column = expr->column;
    col.originalColumn = resCol;
    col.type = Column::COLUMN;

    // Display name
    if (col.alias.isNull())
        col.displayName = expr->column;
    else
        col.displayName = col.alias;

    // Looking for table relation
    Column matched;
    if (isRowIdKeyword(expr->column))
        matched = resolveRowIdColumn(expr);
    else if (!expr->database.isNull())
        matched = resolveExplicitColumn(expr->database, expr->table, expr->column);
    else if (!expr->table.isNull())
        matched = resolveExplicitColumn(expr->table, expr->column);
    else
        matched = resolveExplicitColumn(expr->column);


    if (!matched.table.isNull())
    {
        col.database = matched.database;
        col.originalDatabase = resolveDatabase(matched.database);
        col.table = matched.table;
        col.tableAlias = matched.tableAlias;
        col.flags = matched.flags;
    }
    else if (matched.type == Column::OTHER)
    {
        col.type = Column::OTHER;
    }
    else if (!ignoreInvalidNames)
    {
        QString colStr = expr->detokenize();
        qDebug() << "Source table for column '" << colStr
                 << "' not matched while resolving select: " << query;
        errors << QObject::tr("Could not resolve table for column '%1'.").arg(colStr);
    }

    currentCoreResults << col;
}

SelectResolver::Column SelectResolver::resolveRowIdColumn(SqliteExpr *expr)
{
    // Looking for first source that can provide ROWID.
    for (const Column& column : currentCoreSourceColumns)
    {
        if (column.table.isNull())
            continue; // ROWID cannot be related to source with no table

        if (!expr->table.isNull() && matchTable(column, expr->table))
            return column;
    }
    return Column();
}

SelectResolver::Column SelectResolver::resolveExplicitColumn(const QString &columnName)
{
    for (const Column& column : currentCoreSourceColumns)
    {
        if (columnName.compare(column.column, Qt::CaseInsensitive) != 0 && columnName.compare(column.alias, Qt::CaseInsensitive) != 0)
            continue;

        return column;
    }
    return Column();
}

SelectResolver::Column SelectResolver::resolveExplicitColumn(const QString &table, const QString &columnName)
{
    for (const Column& column : currentCoreSourceColumns)
    {
        if (columnName.compare(column.column, Qt::CaseInsensitive) != 0 && columnName.compare(column.alias, Qt::CaseInsensitive) != 0)
            continue;

        if (!matchTable(column, table))
            continue;

        return column;
    }
    return Column();
}

SelectResolver::Column SelectResolver::resolveExplicitColumn(const QString &database, const QString &table, const QString &columnName)
{
    for (const Column& column : currentCoreSourceColumns)
    {
        if (columnName.compare(column.column, Qt::CaseInsensitive) != 0 && columnName.compare(column.alias, Qt::CaseInsensitive) != 0)
            continue;

        if (!matchTable(column, table))
            continue;

        if (database.compare(column.database, Qt::CaseInsensitive) != 0)
            continue;

        return column;
    }
    return Column();
}

bool SelectResolver::matchTable(const SelectResolver::Column &sourceColumn, const QString &table)
{
    // First check by tableAlias if it's present
    if (!sourceColumn.tableAlias.isNull())
        return (sourceColumn.tableAlias.compare(table, Qt::CaseInsensitive) == 0);

    return (sourceColumn.table.compare(table, Qt::CaseInsensitive) == 0);
}

TokenList SelectResolver::getResColTokensWithoutAlias(SqliteSelect::Core::ResultColumn *resCol)
{
    TokenList allTokens = resCol->tokens;
    if (!resCol->alias.isNull())
    {
        int depth = 0;
        int idx = -1;
        int idxCandidate = -1;
        for (const TokenPtr& token : allTokens)
        {
            idxCandidate++;
            if (token->type == Token::PAR_LEFT)
            {
                depth++;
            }
            else if (token->type == Token::PAR_RIGHT)
            {
                depth--;
            }
            else if (token->type == Token::KEYWORD && token->value.compare("AS", Qt::CaseInsensitive) == 0 && depth <= 0)
            {
                idx = idxCandidate;
                break;
            }
        }

        if (idx > -1)
            allTokens = allTokens.mid(0, idx - 1);
    }

    return allTokens;
}

void SelectResolver::extractCte(SqliteSelect *select)
{
    cteList.clear();
    if (!select->with)
        return;

    for (SqliteWith::CommonTableExpression* cte : select->with->cteList)
        cteList[cte->table] = cte;
}

void SelectResolver::extractCte(SqliteSelect::Core *core)
{
    if (!core->parentStatement())
        return;

    extractCte(dynamic_cast<SqliteSelect*>(core->parentStatement()));
}

QList<SelectResolver::Column> SelectResolver::resolveJoinSource(SqliteSelect::Core::JoinSource *joinSrc)
{
    QList<SelectResolver::Column> columnSources;
    columnSources += resolveSingleSource(joinSrc->singleSource);
    for (SqliteSelect::Core::JoinSourceOther* otherSrc : joinSrc->otherSources)
        columnSources += resolveOtherSource(otherSrc);

    return columnSources;
}

QList<SelectResolver::Column> SelectResolver::resolveSingleSource(SqliteSelect::Core::SingleSource *joinSrc)
{
    if (!joinSrc)
        return QList<Column>();

    if (joinSrc->select)
        return resolveSingleSourceSubSelect(joinSrc);

    if (joinSrc->joinSource)
        return resolveJoinSource(joinSrc->joinSource);

    if (!joinSrc->funcName.isNull())
        return resolveTableFunctionColumns(joinSrc);

    if (isView(joinSrc->database, joinSrc->table))
        return resolveView(joinSrc->database, joinSrc->table, joinSrc->alias);

    if (joinSrc->database.isNull() && cteList.contains(joinSrc->table))
        return resolveCteColumns(joinSrc);

    QList<Column> columnSources;
    QStringList columns = getTableColumns(joinSrc->database, joinSrc->table, joinSrc->alias);
    Column column;
    column.type = Column::COLUMN;
    column.table = joinSrc->table;;
    column.database = joinSrc->database;
    column.originalDatabase = resolveDatabase(joinSrc->database);
    if (!joinSrc->alias.isNull())
        column.tableAlias = joinSrc->alias;

    for (const QString& columnName : columns)
    {
        column.column = columnName;
        columnSources << column;
    }

    return columnSources;
}

QList<SelectResolver::Column> SelectResolver::resolveCteColumns(SqliteSelect::Core::SingleSource* joinSrc)
{
    SqliteWith::CommonTableExpression* cte = cteList[joinSrc->table];

    Column column;
    column.type = Column::COLUMN;
    column.flags |= FROM_CTE_SELECT;
    column.tableAlias = cte->table;

    QList<Column> columnSources;

    static_qstring(cteSelectTpl, "WITH %1 SELECT * FROM %2");
    QString selectQuery = cte->detokenize();
    QString query = cteSelectTpl.arg(selectQuery, cte->table);
    QList<AliasedColumn> queryColumns = db->columnsForQuery(query);
    if (queryColumns.isEmpty())
    {
        qWarning() << "Could not detect query columns. Probably due to db error:" << db->getErrorText();
        return columnSources;
    }

    for (const AliasedColumn& queryColumn : queryColumns)
    {
        if (!queryColumn.getDatabase().isNull())
            column.database = resolveDatabase(queryColumn.getDatabase());
        else
            column.database = queryColumn.getDatabase();

        column.table = queryColumn.getTable();

        // From CTE perspective, however the column is received as "result column name" from SQLite API
        // is what we report back to user of the CTE as available column. No matter if it's actual alias,
        // or simply name of a column.
        column.column = queryColumn.getAlias();
        column.displayName = queryColumn.getAlias();
        columnSources << column;
    }

    return columnSources;
}

QList<SelectResolver::Column> SelectResolver::resolveTableFunctionColumns(SqliteSelect::Core::SingleSource *joinSrc)
{
    static_qstring(columnSqlTpl, "SELECT * FROM %1 LIMIT 0");
    SqlQueryPtr result = db->exec(columnSqlTpl.arg(joinSrc->detokenize()));
    if (result->isError())
        errors << result->getErrorText();

    QStringList columnNames = result->getColumnNames();

    QList<Column> columnSources;
    Column column;
    column.type = Column::OTHER;
    column.database = joinSrc->database;
    column.originalDatabase = resolveDatabase(joinSrc->database);
    if (!joinSrc->alias.isNull())
        column.tableAlias = joinSrc->alias;

    for (const QString& columnName : columnNames)
    {
        column.column = columnName;
        columnSources << column;
    }
    return columnSources;
}

QList<SelectResolver::Column> SelectResolver::resolveSingleSourceSubSelect(SqliteSelect::Core::SingleSource *joinSrc)
{
    QList<Column> columnSources = resolveSubSelect(joinSrc->select);
    applySubSelectAlias(columnSources, joinSrc->alias);

    QMutableListIterator<Column> it(columnSources);
    while (it.hasNext())
    {
        if (it.next().alias.isEmpty())
            continue;

        it.value().aliasDefinedInSubQuery = true;
    }

    return columnSources;
}

QList<SelectResolver::Column> SelectResolver::resolveOtherSource(SqliteSelect::Core::JoinSourceOther *otherSrc)
{
    return resolveSingleSource(otherSrc->singleSource);
}

QList<SelectResolver::Column> SelectResolver::resolveSubSelect(SqliteSelect *select)
{
    QList<Column> columnSources;
    Q_ASSERT(select->coreSelects.size() > 0);

    bool compound = (select->coreSelects.size() > 1);

    if (compound && !resolveMultiCore)
        return columnSources;

    SelectResolver internalResolver(db, query);
    columnSources += internalResolver.resolve(select->coreSelects[0]);

    if (compound)
    {
        QMutableListIterator<Column> it(columnSources);
        while (it.hasNext())
            it.next().flags |= FROM_COMPOUND_SELECT;
    }

    return columnSources;
}

QList<SelectResolver::Column> SelectResolver::resolveView(const QString& database, const QString& name, const QString& alias)
{
    QList<Column> results;
    SqliteQueryPtr query = schemaResolver->getParsedObject(database, name, SchemaResolver::VIEW);
    if (!query)
    {
        qDebug() << "Could not get parsed CREATE VIEW in SelectResolver::resolveView().";
        return results;
    }

    SqliteCreateViewPtr createView = query.dynamicCast<SqliteCreateView>();
    if (!createView)
    {
        qDebug() << "Parsed object not a CREATE VIEW as expected, but instead it's:" << sqliteQueryTypeToString(query->queryType);
        return results;
    }

    results = resolveSubSelect(createView->select);
    applySubSelectAlias(results, (!alias.isNull() ? alias : name));

    return results;
}

bool SelectResolver::isView(const QString& database, const QString& name)
{
    return schemaResolver->getViews(database).contains(name, Qt::CaseInsensitive);
}

QStringList SelectResolver::getTableColumns(const QString &database, const QString &table, const QString& alias)
{
    Table dbTable;
    dbTable.database = database;
    dbTable.table = table;
    dbTable.tableAlias = alias;

    if (tableColumnsCache.contains(dbTable))
    {
        return tableColumnsCache.value(dbTable);
    }
    else
    {
        QStringList columns = schemaResolver->getTableColumns(database, table);
        tableColumnsCache[dbTable] = columns;
        return columns;
    }
}

void SelectResolver::applySubSelectAlias(QList<SelectResolver::Column>& columns, const QString& alias)
{
    // If this subselect is aliased, then all source columns should be considered as from aliased table
    QMutableListIterator<Column> it(columns);
    if (!alias.isNull())
    {
        while (it.hasNext())
        {
            it.next().pushTableAlias();
            it.value().tableAlias = alias;
            it.value().flags &= ~FROM_ANONYMOUS_SELECT; // remove anonymous flag
        }
    }
    else
    {
        // Otherwise, mark column as being from anonymous subselect.
        // This is used by QueryExecutorColumns step to avoid prefixing result column with table
        // when it comes from anonymous subselect (which SQLite needs this to be not prefixed column).
        while (it.hasNext())
            it.next().flags |= FROM_ANONYMOUS_SELECT;
    }
}

QString SelectResolver::resolveDatabase(const QString& database)
{
    if (dbNameToAttach.containsRight(database, Qt::CaseInsensitive))
        return dbNameToAttach.valueByRight(database, Qt::CaseInsensitive);

    return database;
}

bool SelectResolver::parseOriginalQuery()
{
    if (originalQueryParsed)
        return true;

    Parser parser;
    if (!parser.parse(query) || parser.getQueries().isEmpty())
    {
        qWarning() << "Could not parse query in SelectResolver:" << query;
        return false;
    }

    SqliteQueryPtr theQuery = parser.getQueries().first();
    if (theQuery.dynamicCast<SqliteSelect>().isNull())
    {
        qWarning() << "Parsed query is not SELECT as expected in SelectResolver::parseOriginalQuery():" << query;
        return false;
    }

    originalQueryParsed = theQuery.dynamicCast<SqliteSelect>();
    return true;
}

SelectResolver::Table::Table()
{
}

SelectResolver::Table::Table(const SelectResolver::Table &other) :
    database(other.database), originalDatabase(other.originalDatabase), table(other.table),
    tableAlias(other.tableAlias), oldTableAliases(other.oldTableAliases), flags(other.flags)
{
}

int SelectResolver::Table::operator ==(const SelectResolver::Table &other)
{
    return ::operator==(*this, other);
}

void SelectResolver::Table::pushTableAlias()
{
    if (!tableAlias.isNull())
        oldTableAliases += tableAlias;
}

int operator==(const SelectResolver::Table& t1, const SelectResolver::Table& t2)
{
    return t1.table.compare(t2.table, Qt::CaseInsensitive) == 0 &&
           t1.database.compare(t2.database, Qt::CaseInsensitive) == 0 &&
           t1.tableAlias.compare(t2.tableAlias, Qt::CaseInsensitive) == 0 &&
           t1.oldTableAliases.size() == t2.oldTableAliases.size() &&
           t1.oldTableAliases.join(",").compare(t2.oldTableAliases.join(","), Qt::CaseInsensitive) == 0;
}

uint qHash(const SelectResolver::Table& table)
{
    return qHash(table.database.toLower() + "." + table.table.toLower() + "/" + table.tableAlias.toLower() + "/" +
                 table.oldTableAliases.join(","));
}

int SelectResolver::Column::operator ==(const SelectResolver::Column &other)
{
    return ::operator==(*this, other);
}

SelectResolver::Table SelectResolver::Column::getTable() const
{
    return Table(*this);
}

int operator ==(const SelectResolver::Column &c1, const SelectResolver::Column &c2)
{
    return c1.column.compare(c2.column, Qt::CaseInsensitive) == 0 &&
           c1.table.compare(c2.table, Qt::CaseInsensitive) == 0 &&
           c1.database.compare(c2.database, Qt::CaseInsensitive) == 0 &&
           c1.tableAlias.compare(c2.tableAlias, Qt::CaseInsensitive) == 0 &&
           c1.oldTableAliases.size() == c2.oldTableAliases.size() &&
           c1.oldTableAliases.join(",").compare(c2.oldTableAliases.join(","), Qt::CaseInsensitive) == 0;
}

uint qHash(const SelectResolver::Column &column)
{
    return qHash(column.database.toLower() + "." + column.table.toLower() + "." + column.column.toLower() + "/" +
                 column.tableAlias.toLower() + "/" + column.oldTableAliases.join(","));
}
