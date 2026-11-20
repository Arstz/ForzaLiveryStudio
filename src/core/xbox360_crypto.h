#pragma once

#include <QByteArray>
#include <QString>

namespace fls {

enum class ExportGameVersion;

enum class Xbox360KeySource { ProfileKeys, Executable, HorizonTitleUpdate };

struct Xbox360Credentials {
    QByteArray profileId;
    QByteArray keyData;
    Xbox360KeySource source = Xbox360KeySource::ProfileKeys;
};

enum class Xbox360FileKind { Unknown, Livery, Group };
enum class Xbox360Envelope { Unknown, Motorsport2, Sha1, Sha256 };

Xbox360FileKind xbox360FileKind(const QString &fileName);
Xbox360Envelope xbox360Envelope(const QByteArray &bytes);
QByteArray xbox360ProfileId(const QString &hex);
bool xbox360SupportsExecutableKeys(ExportGameVersion game, Xbox360FileKind kind);
QByteArray decryptXbox360Asset(const QByteArray &bytes, ExportGameVersion game,
                              Xbox360FileKind kind, const Xbox360Credentials &credentials);
QByteArray encryptXbox360Asset(const QByteArray &plain, ExportGameVersion game,
                              Xbox360FileKind kind, const Xbox360Credentials &credentials);

} // namespace fls
