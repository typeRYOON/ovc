#include <git/paths.h>
#include <QStandardPaths>

namespace ovc::git {

QString dataRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString reposRoot()
{
    return dataRoot() + QStringLiteral("/repos");
}

QString indexPath()
{
    return dataRoot() + QStringLiteral("/index.json");
}

QString configPath()
{
    return dataRoot() + QStringLiteral("/config.json");
}

} // namespace ovc::git
