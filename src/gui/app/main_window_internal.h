#pragma once

#include "main_window.h"

#include "font_glyphs.h"
#include "import_locations.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace gui::mw_detail {

constexpr int kInitialWindowWidth = 1200;
constexpr int kInitialWindowHeight = 780;
constexpr int kTreeIconExtent = 64;
constexpr int kToolbarIconExtent = 18;
constexpr int kDockSplitterHandleWidth = 6;
constexpr int kDetailsLabelMargin = 10;

inline QString shortcutActionText(const QString &id, const QString &label, const QKeySequence &shortcut) {
    if (shortcut.isEmpty()) {
        return label;
    }
    const QString sequence = shortcut.toString(QKeySequence::NativeText);

    return id.startsWith(QStringLiteral("tool_"))
        ? QStringLiteral("%1 (%2)").arg(label, sequence)
        : QStringLiteral("%1\t%2").arg(label, sequence);
}

inline QString safeGroupName(QString name) {
    name = name.trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("Project");
    }
    for (QChar &ch : name) {
        if (ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char(':') || ch == QLatin1Char('"')
            || ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char('|') || ch == QLatin1Char('?')
            || ch == QLatin1Char('*') || ch.category() == QChar::Other_Control) {
            ch = QLatin1Char('_');
        }
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("Project") : name;
}

inline QString projectExportFolder(const QString &pickedFolder, const QString &projectName, bool livery) {
    const QFileInfo pickedInfo(pickedFolder);
    const QString prefix = livery ? QStringLiteral("Livery_") : QStringLiteral("LayerGroup_");
    if (pickedInfo.fileName().startsWith(prefix) && pickedInfo.fileName().size() > prefix.size()) {
        return pickedInfo.absoluteFilePath();
    }

    QDir base(pickedFolder);
    const QString baseName = QStringLiteral("%1%2").arg(prefix, safeGroupName(projectName));
    QString candidate = baseName;
    int suffix = 2;
    while (base.exists(candidate)) {
        candidate = QStringLiteral("%1_%2").arg(baseName).arg(suffix++);
    }
    return base.filePath(candidate);
}

inline QString importDialogStartDirectoryWithFallbacks(QWidget *parent,
                                                       const QString &actionKey,
                                                       const QStringList &fallbackActionKeys) {
    QSettings settings;
    const auto configuredDirectory = [&](const QString &key) {
        const QString path = settings.value(QStringLiteral("import/%1Directory").arg(key)).toString();
        return !path.isEmpty() && QFileInfo(path).isDir() ? path : QString();
    };

    const QString current = configuredDirectory(actionKey);
    if (!current.isEmpty()) {
        return current;
    }
    for (const QString &fallbackKey : fallbackActionKeys) {
        const QString fallback = configuredDirectory(fallbackKey);
        if (!fallback.isEmpty()) {
            return fallback;
        }
    }
    return importDialogStartDirectory(parent, actionKey);
}

inline bool isProjectDocumentFile(const QFileInfo &info) {
    return info.isFile()
        && info.suffix().compare(QStringLiteral("3so"), Qt::CaseInsensitive) == 0;
}

inline QString entryNameForId(const fls::Project &project, const QString &id) {
    if (!project.root) {
        return {};
    }
    QString name;
    std::function<void(const fls::scene::Layer &)> walk = [&](const fls::scene::Layer &node) {
        if (!name.isEmpty()) {
            return;
        }
        if (node.id == id) {
            name = node.name;
            return;
        }
        if (node.kind() == fls::scene::LayerKind::Group) {
            for (const auto &child : static_cast<const fls::scene::Group &>(node).children) {
                walk(*child);
            }
        }
    };
    for (const auto &child : project.root->children) {
        walk(*child);
    }
    return name;
}

template <typename Fn>
void forEachLayer(fls::Project &project, Fn fn) {
    if (!project.root) {
        return;
    }
    std::function<void(fls::scene::Layer &)> walk = [&](fls::scene::Layer &node) {
        fn(node);
        if (node.kind() == fls::scene::LayerKind::Group) {
            for (const auto &child : static_cast<fls::scene::Group &>(node).children) {
                walk(*child);
            }
        }
    };
    for (const auto &child : project.root->children) {
        walk(*child);
    }
}

template <typename Fn>
void forEachShape(fls::Project &project, Fn fn) {
    forEachLayer(project, [&](fls::scene::Layer &node) {
        if (node.kind() == fls::scene::LayerKind::Shape) {
            fn(static_cast<fls::scene::Shape &>(node));
        }
    });
}

template <typename Fn>
void forEachGuide(fls::Project &project, Fn fn) {
    forEachLayer(project, [&](fls::scene::Layer &node) {
        if (node.kind() == fls::scene::LayerKind::Guide) {
            fn(static_cast<fls::scene::GuideLayer &>(node));
        }
    });
}

inline QVector<fls::scene::Group *> liverySections(fls::Project &project) {
    QVector<fls::scene::Group *> sections;
    if (!project.root) {
        return sections;
    }
    for (const auto &child : project.root->children) {
        if (child->kind() == fls::scene::LayerKind::Group) {
            auto *group = static_cast<fls::scene::Group *>(child.get());
            if (group->isLiverySection) {
                sections.push_back(group);
            }
        }
    }
    return sections;
}

inline QRectF shapeWorldBounds(const fls::scene::Shape &shape, const QSizeF &size) {
    QTransform transform;
    transform.translate(shape.x, shape.y);
    transform.rotate(shape.rotation);
    transform.shear(shape.skew, 0.0);
    transform.scale(shape.scaleX, shape.scaleY);
    const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
    return transform.mapRect(local);
}

struct PlacedTextGlyph {
    int shapeId = -1;
    double originX = 0.0;
};

struct PlacedTextLine {
    QVector<PlacedTextGlyph> glyphs;
    QString skipped;
    double width = 0.0;
};

inline PlacedTextLine layoutTextGlyphs(const QString &fontName,
                                       const QString &text,
                                       bool monospace,
                                       const std::function<QRectF(int)> &inkBounds) {
    constexpr double kGlyphGap = 12.0;
    constexpr double kSpaceWidth = 64.0;

    const auto averageBlockWidth = [&](ushort first, ushort last) {
        double sum = 0.0;
        int count = 0;
        for (ushort u = first; u <= last; ++u) {
            const int id = fontglyphs::glyphShapeId(fontName, QChar(u));
            if (id < 0) {
                continue;
            }
            sum += std::max(inkBounds(id).width(), 1.0);
            ++count;
        }
        return count > 0 ? sum / count : 128.0;
    };

    const double upperCell = monospace ? averageBlockWidth('A', 'Z') : 0.0;
    const double lowerCell = monospace ? averageBlockWidth('a', 'z') : 0.0;
    const double monoSpaceWidth = std::max(upperCell, lowerCell);

    PlacedTextLine line;
    double cursor = 0.0;
    for (const QChar ch : text) {
        if (ch == QLatin1Char(' ')) {
            cursor += monospace ? monoSpaceWidth : kSpaceWidth;
            line.width = cursor;
            continue;
        }

        const int shapeId = fontglyphs::glyphShapeId(fontName, ch);
        if (shapeId < 0) {
            if (!line.skipped.contains(ch)) {
                line.skipped.append(ch);
            }
            continue;
        }

        const QRectF ink = inkBounds(shapeId);
        double originX = 0.0;
        double advance = 0.0;
        if (monospace) {
            const double cell = fontglyphs::isUpperBlockShape(shapeId) ? upperCell : lowerCell;
            originX = cursor + cell * 0.5 - ink.center().x();
            advance = cell;
        } else {
            originX = cursor - ink.left();
            advance = std::max(ink.width(), 1.0);
        }
        line.glyphs.push_back({shapeId, originX});
        cursor += advance;
        line.width = cursor;
        cursor += kGlyphGap;
    }
    return line;
}

} // namespace gui::mw_detail
