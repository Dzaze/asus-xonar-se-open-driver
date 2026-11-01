# Compiler

## DSP portable

CMake 3.25+, Ninja et compilateur C++20. Le pilote et les composants Windows sont
désactivés par défaut sur les plateformes non Windows.

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug --output-on-failure
```

Sous Windows, ouvrir un environnement de développement MSVC x64 et utiliser les
presets `msvc-ninja-release`. Le WDK n’est pas nécessaire pour le DSP.

## Centre de contrôle

Installer le SDK .NET 8 et les outils Windows App SDK nécessaires à WinUI 3.
Les versions des paquets sont épinglées dans le projet. Depuis `apps/control-center` :

```powershell
dotnet build Xonar.ControlCenter.csproj -p:Platform=x64
dotnet run --project ../../tests/ui/shell/Xonar.ControlCenter.Tests.csproj
dotnet publish Xonar.ControlCenter.csproj -c Release -r win-x64 -p:Platform=x64 -p:WindowsAppSDKSelfContained=true -p:WindowsAppSDKBootstrapInitialize=false --self-contained true
```

La publication autonome inclut .NET et Windows App SDK. Les fichiers natifs doivent
rester à côté de l’exécutable ; l’application WinUI n’est pas publiée en single-file.

## Pilote et APO

Installer MSVC x64, le SDK Windows et le WDK. Activer `XONAR_BUILD_DRIVER=ON` pour
construire `xonarse_package`, ou `XONAR_BUILD_APO=ON` pour la DLL. Indiquer
`XONAR_DRIVER_VERSION=0.5.0.1` pour le correctif de provisionnement des effets.
Utiliser un environnement isolé pour les changements noyau.

Signer ses propres builds avec son propre certificat de développement. Aucune
clé privée du développeur n’est incluse dans le dépôt. Les scripts de distribution
et le produit vérifient une empreinte publique : une distribution personnelle doit
mettre à jour cette empreinte et les fichiers de certificat correspondants.

`scripts/release/build-release.ps1` assemble le Setup avec le compilateur .NET
Framework de Windows, à partir d’une application déjà publiée, d’un paquet pilote
signé, de l’APO signée, des licences et du redistribuable Visual C++.
Les chemins par défaut reflètent le banc du développeur ; les arguments permettent
de fournir les entrées de build. Lire le script avant de signer une distribution.
