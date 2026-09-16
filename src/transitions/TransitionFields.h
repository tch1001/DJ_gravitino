// Internal structured view of the portable document, shared by YAML and the
// field inspector so unknown/extension fields cannot disappear from the GUI.
#pragma once
#include <QJsonObject>
#include <QString>

namespace gvt {
struct GvtFile;
QJsonObject transitionDocumentFields(const GvtFile& file);
QString transitionFieldsYaml(const QJsonObject& fields);
}
