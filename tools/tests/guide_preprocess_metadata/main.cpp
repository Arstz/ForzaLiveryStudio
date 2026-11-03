#include "layer.h"
#include "scene_codec.h"

#include <QtCore>

#include <memory>

int main()
{
    fls::scene::Group root;
    root.id = QStringLiteral("__root__");
    auto guide = std::make_unique<fls::scene::GuideLayer>();
    guide->id = QStringLiteral("guide-1");
    guide->preprocessColorCount = 11;
    guide->image = std::make_unique<fls::scene::RasterContainer>();
    guide->image->format = QStringLiteral("svg");
    guide->image->width = 32;
    guide->image->height = 16;
    guide->image->encoded = QByteArrayLiteral("<svg viewBox=\"0 0 32 16\"/>");
    root.append(std::move(guide));

    const QJsonObject encoded = fls::scene::sceneTreeToJson(root);
    const std::unique_ptr<fls::scene::Group> decoded = fls::scene::sceneTreeFromJson(encoded);
    if (!decoded || decoded->children.size() != 1
        || decoded->children.front()->kind() != fls::scene::LayerKind::Guide) {
        qCritical() << "guide metadata round trip did not preserve the guide";
        return 1;
    }
    const auto *decodedGuide = static_cast<const fls::scene::GuideLayer *>(decoded->children.front().get());
    if (decodedGuide->preprocessColorCount != 11) {
        qCritical() << "retained color count was not serialized";
        return 1;
    }
    if (!decodedGuide->imageTopDown) {
        qCritical() << "guide image orientation was not serialized";
        return 1;
    }
    if (!decodedGuide->image || decodedGuide->image->format != QStringLiteral("svg")
        || decodedGuide->image->width != 32 || decodedGuide->image->height != 16
        || decodedGuide->image->encoded != QByteArrayLiteral("<svg viewBox=\"0 0 32 16\"/>")) {
        qCritical() << "SVG guide source was not preserved by serialization";
        return 1;
    }

    QJsonObject legacy = encoded;
    QJsonArray children = legacy.value(QStringLiteral("children")).toArray();
    QJsonObject legacyGuide = children.at(0).toObject();
    QJsonObject legacyImage = legacyGuide.value(QStringLiteral("image")).toObject();
    legacyImage.remove(QStringLiteral("orientation"));
    legacyGuide.insert(QStringLiteral("image"), legacyImage);
    children[0] = legacyGuide;
    legacy.insert(QStringLiteral("children"), children);
    const std::unique_ptr<fls::scene::Group> legacyDecoded =
        fls::scene::sceneTreeFromJson(legacy);
    const auto *legacyDecodedGuide = static_cast<const fls::scene::GuideLayer *>(
        legacyDecoded->children.front().get());
    if (legacyDecodedGuide->imageTopDown) {
        qCritical() << "legacy guide orientation was not preserved";
        return 1;
    }
    return 0;
}
