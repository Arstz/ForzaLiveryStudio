#pragma once

#include <QString>

class QApplication;
class QWidget;

namespace gui {

enum class SystemIconSet {
    Light,
    Dark,
};

enum class ProjectFileAssociationState {
    NotRegistered,
    Registered,
    NeedsRepair,
};

SystemIconSet loadSystemIconSet();
void saveSystemIconSet(SystemIconSet iconSet);
QString systemIconSetSettingsValue(SystemIconSet iconSet);
SystemIconSet systemIconSetFromSettingsValue(const QString &value);
ProjectFileAssociationState projectFileAssociationState(SystemIconSet iconSet);
bool setProjectFileAssociationEnabled(bool enabled,
                                      SystemIconSet iconSet,
                                      QString *error = nullptr);
void configureSystemIntegration(QApplication &app);
void applySystemWindowIcon(QWidget &window);
void offerProjectFileAssociation(QWidget &parent);

} // namespace gui
