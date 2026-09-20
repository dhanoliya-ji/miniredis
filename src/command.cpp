#include "miniredis/command.hpp"

namespace miniredis {

void CommandTable::add(CommandSpec spec) {
    const std::string key = toLower(spec.name);
    m_commands.emplace(key, std::move(spec));
}

const CommandSpec* CommandTable::find(std::string_view name) const {
    const auto it = m_commands.find(toLower(name));
    return it == m_commands.end() ? nullptr : &it->second;
}

std::vector<Bytes> CommandTable::extractKeys(const CommandSpec& spec, const Args& args) const {
    std::vector<Bytes> keys;
    if (spec.firstKey <= 0) return keys;

    const int argc = static_cast<int>(args.size());
    // A negative lastKey counts back from the end, which is how variadic
    // commands such as MSET and DEL describe "every remaining argument".
    const int last = spec.lastKey < 0 ? argc + spec.lastKey : spec.lastKey;
    const int step = spec.keyStep > 0 ? spec.keyStep : 1;

    for (int i = spec.firstKey; i <= last && i < argc; i += step) {
        keys.push_back(args[static_cast<size_t>(i)]);
    }
    return keys;
}

std::string wrongArgsError(std::string_view commandName) {
    return "ERR wrong number of arguments for '" + toLower(commandName) + "' command";
}

CommandTable buildCommandTable() {
    CommandTable table;
    registerConnectionCommands(table);
    registerKeyCommands(table);
    registerStringCommands(table);
    registerListCommands(table);
    registerHashCommands(table);
    registerSetCommands(table);
    registerZSetCommands(table);
    registerServerCommands(table);
    registerPubSubCommands(table);
    registerTransactionCommands(table);
    registerPersistenceCommands(table);
    registerReplicationCommands(table);
    registerClusterCommands(table);
    registerSqlCommands(table);
    return table;
}

} // namespace miniredis
