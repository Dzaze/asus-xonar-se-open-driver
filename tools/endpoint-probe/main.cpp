// endpoint-probe : diagnostic en mode utilisateur de ce que Windows voit des endpoints
// audio de l'ASUS Xonar SE, avec le pilote Microsoft usbaudio2 comme avec xonarse.sys.
//
// Sous-commandes :
//   list  (défaut) inventaire en lecture seule : propriétés MMDevice, formats du moteur,
//         pilote du devnode, jacks KS, volume, format de mixage, formats exclusifs ;
//   tone  sinus sur un canal à la fois, plafonné à -20 dBFS, endpoints Xonar seulement ;
//   watch notifications MMDevice et relecture des jacks chaque seconde.
#include "commands.hpp"
#include "probe_common.hpp"

#include <cstdio>

namespace {

void printGeneralHelp() {
    probe::printOut(
        "endpoint-probe : diagnostic des endpoints audio Windows (Xonar SE Open Driver).\n\n"
        "Usage : endpoint-probe [commande] [options] [--json] [--help]\n\n"
        "Commandes :\n"
        "  list    (défaut) Inventaire en lecture seule des endpoints Xonar (--all : tous).\n"
        "  tone    Joue un sinus sur un canal d'un endpoint Xonar (plafond -20 dBFS).\n"
        "  watch   Suit les changements d'état des endpoints Xonar et de leurs jacks.\n\n"
        "Options globales :\n"
        "  --json  Sortie JSON sur stdout (messages humains sur stderr).\n"
        "  --help  Aide générale, ou aide d'une commande : endpoint-probe <commande> --help.\n\n"
        "Codes de retour : 0 OK, 2 aucun endpoint Xonar, 1 erreur.");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    probe::CommandArgs args;
    std::wstring command;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--json") args.json = true;
        else if (a == L"--help" || a == L"-h" || a == L"/?") args.help = true;
        else if (command.empty() && !a.empty() && a[0] != L'-') command = a;
        else args.args.push_back(a);
    }

    if (command.empty() && args.help) {
        printGeneralHelp();
        return probe::kExitOk;
    }
    if (command.empty() || command == L"list") return probe::cmdList(args);
    if (command == L"tone") return probe::cmdTone(args);
    if (command == L"watch") return probe::cmdWatch(args);
    if (command == L"help") {
        printGeneralHelp();
        return probe::kExitOk;
    }
    probe::printErr("Commande inconnue : " + probe::toUtf8(command) + " (voir endpoint-probe --help)");
    return probe::kExitError;
}
