// endpoint-probe : points d'entrée des sous-commandes.
#pragma once

#include <string>
#include <vector>

namespace probe {

inline constexpr const char* kToolVersion = "0.1.0";

// Arguments restants après le nom de la commande (les options globales --json et
// --help sont déjà retirées et transmises séparément).
struct CommandArgs {
    std::vector<std::wstring> args;
    bool json = false;
    bool help = false;
};

int cmdList(const CommandArgs& a);
int cmdTone(const CommandArgs& a);
int cmdWatch(const CommandArgs& a);

void printListHelp();
void printToneHelp();
void printWatchHelp();

} // namespace probe
