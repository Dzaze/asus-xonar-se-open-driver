// SPDX-License-Identifier: Apache-2.0
// Commande minimale (ICommand) pour relier une action aux commandes de l'icône de
// notification (H.NotifyIcon). Pas de dépendance MVVM externe pour si peu.

using System;
using System.Windows.Input;

namespace Xonar.ControlCenter.Services;

public sealed class RelayCommand : ICommand
{
    private readonly Action _execute;

    public RelayCommand(Action execute) => _execute = execute;

    public event EventHandler? CanExecuteChanged;

    public bool CanExecute(object? parameter) => true;

    public void Execute(object? parameter) => _execute();
}
