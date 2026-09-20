// A small SQL surface over the keyspace.
//
// This is the one part of MiniRedis that Redis has no equivalent for. It is
// carried forward from the project this grew out of, where the whole database
// was addressed through SQL-ish statements, and it is kept for two reasons:
//
//  1. It is genuinely convenient at a terminal. `SELECT * FROM kv` printing an
//     aligned table beats reading a RESP array by eye.
//
//  2. It makes a real point about interface design. The keyspace underneath is
//     a hash map -- there is no schema, no join, no secondary index and no
//     query planner. SQL over a key-value store can only ever express what the
//     storage engine can already do. `WHERE key = 'x'` is a hash lookup;
//     `WHERE value = 'x'` would be a full scan, which is exactly why it is not
//     supported. The translation layer cannot invent capabilities the engine
//     does not have, and that is the useful lesson.
//
// Supported grammar:
//
//   SELECT * FROM kv [LIMIT n]
//   SELECT value FROM kv WHERE key = 'k'
//   INSERT INTO kv [(key, value)] VALUES ('k', 'v')
//   UPDATE kv SET value = 'v' WHERE key = 'k'
//   DELETE FROM kv WHERE key = 'k'
//   SHOW TABLES / DESCRIBE kv
#include "command_helpers.hpp"

#include <algorithm>
#include <sstream>

namespace miniredis {

using namespace detail;

namespace {

enum class SqlVerb { Select, SelectAll, Insert, Update, Delete, Show, Describe, Invalid };

struct SqlStatement {
    SqlVerb verb = SqlVerb::Invalid;
    std::string key;
    std::string value;
    std::int64_t limit = -1;
    std::string error;
};

// Extracts single- or double-quoted literals, honouring backslash escapes.
std::vector<std::string> extractLiterals(std::string_view text) {
    std::vector<std::string> literals;
    std::string current;
    char quote = 0;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < text.size()) {
                current.push_back(text[++i]);
            } else if (c == quote) {
                literals.push_back(current);
                current.clear();
                quote = 0;
            } else {
                current.push_back(c);
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        }
    }
    return literals;
}

SqlStatement parseStatement(std::string_view raw) {
    SqlStatement statement;

    std::string text = trim(raw);
    while (!text.empty() && text.back() == ';') {
        text.pop_back();
        text = trim(text);
    }
    if (text.empty()) {
        statement.error = "empty statement";
        return statement;
    }

    const std::string upper = toUpper(text);

    if (upper.rfind("SHOW", 0) == 0) {
        statement.verb = SqlVerb::Show;
        return statement;
    }
    if (upper.rfind("DESCRIBE", 0) == 0 || upper.rfind("DESC ", 0) == 0) {
        statement.verb = SqlVerb::Describe;
        return statement;
    }

    if (upper.rfind("SELECT", 0) == 0) {
        const size_t wherePos = upper.find("WHERE");

        if (wherePos == std::string::npos) {
            // No WHERE clause means a full scan of the keyspace, which is
            // exactly the operation that is cheap to write and expensive to
            // run, so LIMIT is supported to bound it.
            statement.verb = SqlVerb::SelectAll;
            const size_t limitPos = upper.find("LIMIT");
            if (limitPos != std::string::npos) {
                std::int64_t limit = 0;
                if (!parseInt64(trim(text.substr(limitPos + 5)), limit) || limit < 0) {
                    statement.verb = SqlVerb::Invalid;
                    statement.error = "LIMIT expects a non-negative integer";
                }
                statement.limit = limit;
            }
            return statement;
        }

        const std::string whereClause = text.substr(wherePos);
        const std::string upperWhere = toUpper(whereClause);

        // Only `key = '...'` is answerable. Anything else would need a scan or
        // an index that does not exist, and quietly turning a query into a
        // full scan is how a fast system becomes a slow one.
        if (upperWhere.find("KEY") == std::string::npos) {
            statement.error =
                "only 'WHERE key = ...' is supported. The keyspace is a hash map with no "
                "secondary index, so filtering on value would require scanning every key";
            return statement;
        }

        const auto literals = extractLiterals(whereClause);
        if (literals.empty()) {
            statement.error = "the key in a WHERE clause must be quoted, e.g. WHERE key = 'user:1'";
            return statement;
        }

        statement.verb = SqlVerb::Select;
        statement.key = literals.front();
        return statement;
    }

    if (upper.rfind("INSERT", 0) == 0) {
        const size_t valuesPos = upper.find("VALUES");
        if (valuesPos == std::string::npos) {
            statement.error = "INSERT requires a VALUES clause";
            return statement;
        }
        const auto literals = extractLiterals(text.substr(valuesPos));
        if (literals.size() < 2) {
            statement.error = "INSERT VALUES needs a quoted key and a quoted value";
            return statement;
        }
        statement.verb = SqlVerb::Insert;
        statement.key = literals[0];
        statement.value = literals[1];
        return statement;
    }

    if (upper.rfind("UPDATE", 0) == 0) {
        const size_t setPos = upper.find("SET");
        const size_t wherePos = upper.find("WHERE");
        if (setPos == std::string::npos || wherePos == std::string::npos || wherePos < setPos) {
            statement.error = "UPDATE requires SET followed by WHERE";
            return statement;
        }

        const auto setLiterals = extractLiterals(text.substr(setPos, wherePos - setPos));
        const auto whereLiterals = extractLiterals(text.substr(wherePos));
        if (setLiterals.empty() || whereLiterals.empty()) {
            statement.error = "UPDATE needs a quoted value in SET and a quoted key in WHERE";
            return statement;
        }

        statement.verb = SqlVerb::Update;
        statement.value = setLiterals.front();
        statement.key = whereLiterals.front();
        return statement;
    }

    if (upper.rfind("DELETE", 0) == 0) {
        const size_t wherePos = upper.find("WHERE");
        if (wherePos == std::string::npos) {
            // Refusing an unqualified DELETE is deliberate: FLUSHDB exists and
            // says what it does, whereas `DELETE FROM kv` silently destroying
            // a database is the classic production accident.
            statement.error = "DELETE requires a WHERE clause; use FLUSHDB to empty the database";
            return statement;
        }
        const auto literals = extractLiterals(text.substr(wherePos));
        if (literals.empty()) {
            statement.error = "DELETE needs a quoted key in its WHERE clause";
            return statement;
        }
        statement.verb = SqlVerb::Delete;
        statement.key = literals.front();
        return statement;
    }

    statement.error = "unsupported statement; MiniRedis SQL understands SELECT, INSERT, UPDATE, "
                      "DELETE, SHOW TABLES and DESCRIBE";
    return statement;
}

// Renders rows as an aligned ASCII table, which is the whole reason anybody
// would prefer this interface at a terminal.
std::string renderTable(const std::vector<std::pair<std::string, std::string>>& rows) {
    if (rows.empty()) return "Empty set.\n";

    size_t keyWidth = 3;   // "key"
    size_t valueWidth = 5; // "value"
    for (const auto& [key, value] : rows) {
        keyWidth = std::max(keyWidth, key.size());
        valueWidth = std::max(valueWidth, value.size());
    }

    // A single enormous value would produce a table wider than any terminal,
    // so long values are elided rather than wrapped.
    constexpr size_t kMaxColumnWidth = 60;
    keyWidth = std::min(keyWidth, kMaxColumnWidth);
    valueWidth = std::min(valueWidth, kMaxColumnWidth);

    auto fit = [](const std::string& text, size_t width) {
        if (text.size() <= width) return text + std::string(width - text.size(), ' ');
        return text.substr(0, width - 3) + "...";
    };

    const std::string separator =
        "+" + std::string(keyWidth + 2, '-') + "+" + std::string(valueWidth + 2, '-') + "+\n";

    std::string out = separator;
    out += "| " + fit("key", keyWidth) + " | " + fit("value", valueWidth) + " |\n";
    out += separator;
    for (const auto& [key, value] : rows) {
        out += "| " + fit(key, keyWidth) + " | " + fit(value, valueWidth) + " |\n";
    }
    out += separator;
    out += formatInt64(static_cast<std::int64_t>(rows.size())) + " row(s) in set.\n";
    return out;
}

void cmdSql(CommandContext& ctx) {
    // The statement may arrive as one quoted argument or as loose words, so
    // the arguments are rejoined before parsing.
    std::string text;
    for (size_t i = 1; i < ctx.args.size(); ++i) {
        if (!text.empty()) text.push_back(' ');
        text += ctx.args[i];
    }

    const SqlStatement statement = parseStatement(text);
    if (statement.verb == SqlVerb::Invalid) {
        ctx.reply().error("ERR SQL: " + statement.error);
        return;
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);

    switch (statement.verb) {
        case SqlVerb::Show:
            ctx.reply().bulkString(
                "+--------+\n| table  |\n+--------+\n| kv     |\n+--------+\n1 row in set.\n");
            return;

        case SqlVerb::Describe:
            ctx.reply().bulkString(
                "+-------+--------+------+-----+\n"
                "| field | type   | null | key |\n"
                "+-------+--------+------+-----+\n"
                "| key   | string | NO   | PRI |\n"
                "| value | string | NO   |     |\n"
                "+-------+--------+------+-----+\n"
                "The kv table is the string keys of the current database. Keys of other types\n"
                "are not shown, because a list or a hash has no single value column.\n");
            return;

        case SqlVerb::Select: {
            Object* value = keyspace.lookupRead(statement.key, ctx.now);
            if (value == nullptr) {
                ctx.reply().bulkString("Empty set.\n");
                return;
            }
            if (value->type() != ObjectType::String) {
                ctx.reply().error("ERR SQL: key '" + statement.key + "' holds a " +
                                  value->typeName() + ", which has no single value column");
                return;
            }
            ctx.reply().bulkString(renderTable({{statement.key, value->string()}}));
            return;
        }

        case SqlVerb::SelectAll: {
            std::vector<std::pair<std::string, std::string>> rows;
            keyspace.forEach([&](const Bytes& key, const KeyEntry& entry) {
                if (statement.limit >= 0 && static_cast<std::int64_t>(rows.size()) >= statement.limit) return;
                if (entry.value.type() != ObjectType::String) return;
                rows.emplace_back(key, entry.value.string());
            });
            // Sorted so that repeated runs of the same query agree; the hash
            // map's own order is arbitrary and changes on rehash.
            std::sort(rows.begin(), rows.end());
            ctx.reply().bulkString(renderTable(rows));
            return;
        }

        case SqlVerb::Insert: {
            if (keyspace.exists(statement.key, ctx.now)) {
                ctx.reply().error("ERR SQL: key '" + statement.key +
                                  "' already exists; use UPDATE to change it");
                ctx.suppressPropagation();
                return;
            }
            keyspace.setValue(statement.key, Object::makeString(statement.value));
            touchKey(ctx, statement.key);
            ctx.reply().bulkString("Query OK, 1 row affected (INSERT).\n");
            // Propagated as the equivalent SET so that a replica does not have
            // to carry a SQL parser, and so the AOF stays one uniform format.
            ctx.propagateInstead(Args{"SET", statement.key, statement.value});
            return;
        }

        case SqlVerb::Update: {
            Object* existing = keyspace.lookupWrite(statement.key, ctx.now);
            if (existing == nullptr) {
                ctx.reply().error("ERR SQL: key '" + statement.key +
                                  "' does not exist; use INSERT to create it");
                ctx.suppressPropagation();
                return;
            }
            if (existing->type() != ObjectType::String) {
                ctx.reply().error("ERR SQL: key '" + statement.key + "' holds a " +
                                  existing->typeName() + ", which UPDATE cannot set");
                ctx.suppressPropagation();
                return;
            }
            keyspace.setValueKeepTtl(statement.key, Object::makeString(statement.value));
            touchKey(ctx, statement.key);
            ctx.reply().bulkString("Query OK, 1 row affected (UPDATE).\n");
            ctx.propagateInstead(Args{"SET", statement.key, statement.value, "KEEPTTL"});
            return;
        }

        case SqlVerb::Delete: {
            if (!keyspace.erase(statement.key)) {
                ctx.reply().bulkString("Query OK, 0 rows affected (key not found).\n");
                ctx.suppressPropagation();
                return;
            }
            touchKey(ctx, statement.key);
            ctx.reply().bulkString("Query OK, 1 row affected (DELETE).\n");
            ctx.propagateInstead(Args{"DEL", statement.key});
            return;
        }

        case SqlVerb::Invalid:
            break;
    }

    ctx.reply().error("ERR SQL: " + statement.error);
}

} // namespace

void registerSqlCommands(CommandTable& table) {
    using namespace cmdflag;
    table.add({"SQL", cmdSql, -2, kWrite | kDenyOom, 0, 0, 0});
}

} // namespace miniredis
