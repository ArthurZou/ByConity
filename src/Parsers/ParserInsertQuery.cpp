/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/formatAST.h>

#include <Parsers/CommonParsers.h>
#include <Parsers/ExpressionElementParsers.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/ParserSelectWithUnionQuery.h>
#include <Parsers/ParserWatchQuery.h>
#include <Parsers/ParserInsertQuery.h>
#include <Parsers/ParserSetQuery.h>
#include <Parsers/ParserPartition.h>
#include <Parsers/InsertQuerySettingsPushDownVisitor.h>
#include <Common/typeid_cast.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int SYNTAX_ERROR;
}


bool ParserInsertQuery::parseImpl(Pos & pos, ASTPtr & node, Expected & expected)
{
    ParserKeyword s_insert_ignore("INSERT IGNORE INTO");
    ParserKeyword s_insert_overwrite("INSERT OVERWRITE");
    ParserKeyword s_replace_into("REPLACE INTO");
    ParserKeyword s_insert_into("INSERT INTO");
    ParserKeyword s_on_duplicate_key_update("ON DUPLICATE KEY UPDATE");

    ParserKeyword s_table("TABLE");
    ParserKeyword s_function("FUNCTION");
    ParserToken s_dot(TokenType::Dot);
    ParserKeyword s_values("VALUES");
    ParserKeyword s_format("FORMAT");
    ParserKeyword s_settings("SETTINGS");
    ParserKeyword s_select("SELECT");
    ParserKeyword s_watch("WATCH");
    ParserKeyword s_with("WITH");
    ParserKeyword s_infile("INFILE");
    ParserKeyword s_compression("COMPRESSION");
    ParserKeyword s_partition_by("PARTITION BY");
    ParserKeyword s_overwrite_partition("PARTITION");
    ParserPartition parser_overwrite_partition(dt);
    ParserToken s_lparen(TokenType::OpeningRoundBracket);
    ParserToken s_rparen(TokenType::ClosingRoundBracket);
    ParserIdentifier name_p;
    ParserList columns_p(std::make_unique<ParserInsertElement>(dt), std::make_unique<ParserToken>(TokenType::Comma), false);
    ParserFunction table_function_p{dt, false};
    ParserExpressionWithOptionalAlias exp_elem_p(false, ParserSettings::CLICKHOUSE);

    /// create ASTPtr variables (result of parsing will be put in them).
    /// They will be used to initialize ASTInsertQuery's fields.
    ASTPtr database;
    ASTPtr table;
    ASTPtr columns;
    ASTPtr format;
    ASTPtr select;
    ASTPtr watch;
    ASTPtr table_function;
    ASTPtr in_file;
    ASTPtr compression;
    ASTPtr settings_ast;
    ASTPtr partition_by_expr;
    ASTPtr overwrite_partition;
    ASTPtr on_duplicate_assignments;
    bool is_overwrite {false};
    bool is_replace {false};
    bool insert_ignore {false};
    bool on_duplicate_key_parsed {false};

    /// Insertion data
    const char * data = nullptr;

    // Check for key words `INSERT INTO` or `REPLACE INTO` or `INSERT OVERWRITE`. If it isn't found, the query can't be parsed as insert query.
    if (s_insert_ignore.ignore(pos, expected))
        insert_ignore = true;
    else if (s_insert_overwrite.ignore(pos, expected))
        is_overwrite = true;
    else if (s_replace_into.ignore(pos, expected))
        is_replace = true;
    else
    {
        if (!s_insert_into.ignore(pos, expected))
            return false;
    }

    // try to find 'TABLE'
    s_table.ignore(pos, expected);

    /// Search for 'FUNCTION'. If this key word is in query, read fields for insertion into 'TABLE FUNCTION'.
    /// Word table is optional for table functions. (for example, s3 table function)
    /// Otherwise fill 'TABLE' fields.
    if (s_function.ignore(pos, expected))
    {
        /// Read function name
        if (!table_function_p.parse(pos, table_function, expected))
            return false;

        /// Support insertion values with partition by.
        if (s_partition_by.ignore(pos, expected))
        {
            if (!exp_elem_p.parse(pos, partition_by_expr, expected))
                return false;
        }
    }
    else
    {
        /// Read one word. It can be table or database name.
        if (!name_p.parse(pos, table, expected))
            return false;

        /// If there is a dot, previous name was database name,
        /// so read table name after dot.
        if (s_dot.ignore(pos, expected))
        {
            database = table;
            tryRewriteCnchDatabaseName(database, pos.getContext());
            if (!name_p.parse(pos, table, expected))
                return false;
        }
    }

    /// insert overwrite partition
    if (is_overwrite && s_overwrite_partition.ignore(pos, expected))
    {
        if (!s_lparen.ignore(pos, expected))
            return false;

        if (!ParserList{std::make_unique<ParserPartition>(), std::make_unique<ParserToken>(TokenType::Comma), false}.parse(
                pos, overwrite_partition, expected))
            return false;

        if (!s_rparen.ignore(pos, expected))
            return false;
    }

    /// Is there a list of columns
    if (s_lparen.ignore(pos, expected))
    {
        if (!columns_p.parse(pos, columns, expected))
            return false;

        if (!s_rparen.ignore(pos, expected))
            return false;
    }

    Pos before_values = pos;

    /// ON DUPLICATE KEY UPDATE
    /// For INSERT INTO TABLE ON DUPLICATE KEY UPDATE on_duplicate_action VALUES
    if (s_on_duplicate_key_update.ignore(pos, expected))
    {
        on_duplicate_key_parsed = true;
        ParserList parser_assignment_list(std::make_unique<ParserAssignmentWithAlias>(dt), std::make_unique<ParserToken>(TokenType::Comma), false);
        if (!parser_assignment_list.parse(pos, on_duplicate_assignments, expected))
            return false;
    }

    /// VALUES or FORMAT or SELECT
    if (s_values.ignore(pos, expected))
    {
        /// If VALUES is defined in query, everything except setting will be parsed as data
        data = pos->begin;
    }
    else if (s_format.ignore(pos, expected))
    {
        /// If FORMAT is defined, read format name
        if (!name_p.parse(pos, format, expected))
            return false;

        if (s_infile.ignore(pos, expected))
        {
            if (!ParserStringLiteral().parse(pos, in_file, expected))
                return false;
        }

        /// Check for 'COMPRESSION' parameter (optional)
        if (s_compression.ignore(pos, expected))
        {
            /// Read compression name. Create parser for this purpose.
            ParserStringLiteral compression_p;
            if (!compression_p.parse(pos, compression, expected))
                return false;
        }
    }
    else if (s_select.ignore(pos, expected) || s_with.ignore(pos,expected))
    {
        /// If SELECT is defined, return to position before select and parse
        /// rest of query as SELECT query.
        pos = before_values;
        ParserSelectWithUnionQuery select_p(dt);
        select_p.parse(pos, select, expected);

        /// FORMAT section is expected if we have input() in SELECT part
        if (s_format.ignore(pos, expected) && !name_p.parse(pos, format, expected))
            return false;
    }
    /// Check if file is a source of data.
    else if (s_infile.ignore(pos, expected))
    {
        /// Read file name to process it later
        if (!ParserStringLiteral().parse(pos, in_file, expected))
            return false;

        /// Check for 'COMPRESSION' parameter (optional)
        if (s_compression.ignore(pos, expected))
        {
            /// Read compression name. Create parser for this purpose.
            ParserStringLiteral compression_p;
            if (!compression_p.parse(pos, compression, expected))
                return false;
        }
    }
    else if (s_watch.ignore(pos, expected))
    {
        pos = before_values;
        ParserWatchQuery watch_p;
        watch_p.parse(pos, watch, expected);

        /// FORMAT section is expected if we have input() in SELECT part
        if (s_format.ignore(pos, expected) && !name_p.parse(pos, format, expected))
            return false;
    }
    else
    {
        /// If all previous conditions were false, query is incorrect
        return false;
    }

    /// ON DUPLICATE KEY UPDATE
    /// For INSERT INTO TABLE SELECT xxx ON DUPLICATE KEY UPDATE on_duplicate_action
    if (s_on_duplicate_key_update.ignore(pos, expected))
    {
        /// ON DUPLICATE KEY UPDATE cannot be parsed twice
        if (on_duplicate_key_parsed)
            return false;

        ParserList parser_assignment_list(std::make_unique<ParserAssignmentWithAlias>(dt), std::make_unique<ParserToken>(TokenType::Comma), false);
        if (!parser_assignment_list.parse(pos, on_duplicate_assignments, expected))
            return false;
    }

    /// Read SETTINGS if they are defined
    if (s_settings.ignore(pos, expected))
    {
        /// Settings are written like SET query, so parse them with ParserSetQuery
        ParserSetQuery parser_settings(true);
        if (!parser_settings.parse(pos, settings_ast, expected))
            return false;
    }

    if (select)
    {
        /// Copy SETTINGS from the INSERT ... SELECT ... SETTINGS
        InsertQuerySettingsPushDownVisitor::Data visitor_data{settings_ast};
        InsertQuerySettingsPushDownVisitor(visitor_data).visit(select);
    }

    /// In case of defined format, data follows it.
    if (!in_file && format)
    {
        Pos last_token = pos;
        --last_token;
        data = last_token->end;

        /// If format name is followed by ';' (end of query symbol) there is no data to insert.
        if (data < end && *data == ';')
            throw Exception("You have excessive ';' symbol before data for INSERT.\n"
                                    "Example:\n\n"
                                    "INSERT INTO t (x, y) FORMAT TabSeparated\n"
                                    ";\tHello\n"
                                    "2\tWorld\n"
                                    "\n"
                                    "Note that there is no ';' just after format name, "
                                    "you need to put at least one whitespace symbol before the data.", ErrorCodes::SYNTAX_ERROR);

        while (data < end && (*data == ' ' || *data == '\t' || *data == '\f'))
            ++data;

        /// Data starts after the first newline, if there is one, or after all the whitespace characters, otherwise.

        if (data < end && *data == '\r')
            ++data;

        if (data < end && *data == '\n')
            ++data;
    }

    /// Create query and fill its fields.
    auto query = std::make_shared<ASTInsertQuery>();
    node = query;

    if (table_function)
    {
        query->table_function = table_function;
        query->partition_by = partition_by_expr;
    }
    else
    {
        tryGetIdentifierNameInto(database, query->table_id.database_name);
        tryGetIdentifierNameInto(table, query->table_id.table_name);
    }

    tryGetIdentifierNameInto(format, query->format);

    query->insert_ignore = insert_ignore;
    query->is_overwrite = is_overwrite;
    query->is_replace = is_replace;
    query->overwrite_partition = overwrite_partition;
    query->columns = columns;
    query->select = select;
    query->watch = watch;
    if (on_duplicate_assignments)
    {
        if (settings_ast)
        {
            auto *settings_to_change = settings_ast->as<ASTSetQuery>();
            settings_to_change->changes.push_back({"on_duplicate_action", DB::serializeAST(*on_duplicate_assignments)});
        }
        else
        {
            auto setting_node = std::make_shared<ASTSetQuery>();

            SettingsChanges changes;
            setting_node->is_standalone = false;
            changes.push_back({"on_duplicate_action", DB::serializeAST(*on_duplicate_assignments)});
            setting_node->changes = std::move(changes);

            settings_ast = setting_node;
        }
    }
    query->settings_ast = settings_ast;
    query->data = data != end ? data : nullptr;
    query->end = end;

    if (in_file)
    {
        query->in_file = in_file;
        query->data = nullptr;

        query->children.push_back(in_file);
        if (compression)
        {
            query->compression = compression;
            query->children.push_back(compression);
        }
    }

    if (columns)
        query->children.push_back(columns);
    if (select)
        query->children.push_back(select);
    if (watch)
        query->children.push_back(watch);
    if (settings_ast)
        query->children.push_back(settings_ast);
    if (overwrite_partition)
        query->children.push_back(overwrite_partition);
    return true;
}

bool ParserInsertElement::parseImpl(Pos & pos, ASTPtr & node, Expected & expected)
{
    return ParserColumnsMatcher(dt).parse(pos, node, expected)
        || ParserQualifiedAsterisk(dt).parse(pos, node, expected)
        || ParserAsterisk(dt).parse(pos, node, expected)
        || ParserCompoundIdentifier().parse(pos, node, expected);
}

}
