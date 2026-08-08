#pragma once
#include <QString>

namespace ovc::git {

// %LOCALAPPDATA%/ovc — requires QCoreApplication::setApplicationName("ovc").
QString dataRoot();
QString reposRoot();  // dataRoot()/repos
QString indexPath();  // dataRoot()/index.json
QString configPath(); // dataRoot()/config.json

} // namespace ovc::git
