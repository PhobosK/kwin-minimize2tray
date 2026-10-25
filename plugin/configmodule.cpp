#include "rules.h"

#include <KCModule>
#include <KConfigGroup>
#include <KPluginFactory>
#include <KService>
#include <KSharedConfig>
#include <QAbstractListModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLockFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

using namespace Minimize2Tray;

namespace {
template <typename Handler> void whenFinished(QDBusPendingCallWatcher *watcher, QObject *context, Handler handler) {
    // Disconnected or immediately completed calls may finish before a signal can be connected.
    if (watcher->isFinished())
        QTimer::singleShot(0, context, [watcher, handler]() { handler(watcher); });
    else
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, handler);
}

QLabel *plainLabel(const QString &text, QWidget *parent = nullptr) {
    auto label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

void setMessage(QLabel *label, const QString &text) {
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

QLabel *sectionLabel(const QString &text) {
    auto label = plainLabel(text);
    auto font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QPushButton *makeButton(const QString &text, const QString &icon, QWidget *parent = nullptr) {
    auto button = new QPushButton(QIcon::fromTheme(icon), text, parent);
    return button;
}

KConfigGroup uiState(const QString &name) {
    return KSharedConfig::openConfig(QStringLiteral("kwinrc"))
        ->group(QString::fromLatin1(ConfigGroup))
        .group(QStringLiteral("Ui"))
        .group(name);
}

void rememberGeometry(QWidget *window, const KConfigGroup &state) {
    if (auto dialog = qobject_cast<QDialog *>(window)) {
        QObject::connect(dialog, &QDialog::finished, dialog, [dialog, state]() {
            auto saved = state;
            saved.writeEntry("geometry", dialog->saveGeometry());
            saved.sync();
        });
    }
}

void restoreGeometryLater(QWidget *window, const KConfigGroup &state) {
    QTimer::singleShot(0, window, [window, state]() {
        const auto geometry = state.readEntry("geometry", QByteArray());
        if (!geometry.isEmpty() && geometry.size() <= 4096)
            window->restoreGeometry(geometry);
    });
}

void rememberDialog(QDialog *dialog, const QString &name, const QSize &size) {
    dialog->setWindowRole(QStringLiteral("minimize2tray-") + name);
    dialog->setMinimumSize(580, 420);
    const auto available =
        (dialog->screen()->availableGeometry().size() - QSize(32, 64)).expandedTo(dialog->minimumSize());
    dialog->resize(size.boundedTo(available));
    rememberGeometry(dialog, uiState(name));
    restoreGeometryLater(dialog, uiState(name));
}

QString appName(const QVariantMap &info) {
    const QString id = desktopIdentity(info.value(QStringLiteral("desktopFile")).toString());
    const auto service = id.isEmpty() || id.contains(QLatin1Char('/'))
                             ? KService::Ptr()
                             : KService::serviceByStorageId(id + QStringLiteral(".desktop"));
    if (service && service->isApplication() && !service->name().isEmpty())
        return service->name();
    const auto className = info.value(QStringLiteral("resourceClass")).toString();
    if (!className.isEmpty())
        return className;
    const auto instance = info.value(QStringLiteral("resourceName")).toString();
    return instance.isEmpty() ? id : instance;
}

QString fieldLabel(const QString &key) {
    if (key == QLatin1String("desktopFile"))
        return QObject::tr("Application ID");
    if (key == QLatin1String("resourceClass"))
        return QObject::tr("Window class");
    if (key == QLatin1String("resourceName"))
        return QObject::tr("Window instance");
    if (key == QLatin1String("legacyClass"))
        return QObject::tr("Class or instance (imported)");
    if (key == QLatin1String("role"))
        return QObject::tr("Window role");
    return QObject::tr("Window title");
}

const QStringList fields{QStringLiteral("desktopFile"), QStringLiteral("resourceClass"), QStringLiteral("resourceName"),
                         QStringLiteral("legacyClass"), QStringLiteral("role"),          QStringLiteral("caption")};

class RuleModel : public QAbstractListModel {
  public:
    enum { RuleRole = Qt::UserRole + 1, SearchRole };
    QVariantList rules;
    explicit RuleModel(QObject *parent) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : rules.size(); }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= rules.size())
            return {};
        const auto rule = rules[index.row()].toMap();
        switch (role) {
        case RuleRole:
            return rule;
        case SearchRole:
            return searchText(rule);
        case Qt::DisplayRole:
            return rule.value(QStringLiteral("title"));
        case Qt::ToolTipRole:
            return searchText(rule);
        case Qt::CheckStateRole:
            return rule.value(QStringLiteral("enabled")).toBool() ? Qt::Checked : Qt::Unchecked;
        case Qt::AccessibleTextRole:
            return rule.value(QStringLiteral("title"));
        case Qt::AccessibleDescriptionRole:
            return searchText(rule) + tr(". Space: enable or disable. F2: edit. Delete: remove.");
        default:
            return {};
        }
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        return index.isValid() ? Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable : Qt::NoItemFlags;
    }
    bool setData(const QModelIndex &index, const QVariant &value, int role) override {
        if (!index.isValid() || index.row() >= rules.size() || role != Qt::CheckStateRole)
            return false;
        auto rule = rules[index.row()].toMap();
        rule[QStringLiteral("enabled")] = value.toInt() == Qt::Checked;
        rules[index.row()] = rule;
        Q_EMIT dataChanged(index, index);
        return true;
    }
    void reset(const QVariantList &values) {
        beginResetModel();
        rules = values;
        endResetModel();
    }
    int find(const QString &id) const {
        for (int i = 0; i < rules.size(); ++i) {
            if (rules[i].toMap().value(QStringLiteral("id")) == id)
                return i;
        }
        return -1;
    }
    void put(const QVariantMap &rule, int at = -1) {
        const int existing = find(rule.value(QStringLiteral("id")).toString());
        if (existing >= 0) {
            rules[existing] = rule;
            Q_EMIT dataChanged(index(existing), index(existing));
        } else {
            at = at < 0 ? rules.size() : std::min(at, int(rules.size()));
            beginInsertRows({}, at, at);
            rules.insert(at, rule);
            endInsertRows();
        }
    }
    void remove(int at) {
        beginRemoveRows({}, at, at);
        rules.removeAt(at);
        endRemoveRows();
    }
};

class RuleDelegate : public QStyledItemDelegate {
    Q_OBJECT
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return QSize(760, std::max(102, option.fontMetrics.height() * 3 + 34));
    }
    static QRect deleteRect(const QRect &r, const QFontMetrics &fm) {
        const int size = std::max(32, fm.height() + 12);
        return QRect(r.right() - size - 12, r.center().y() - size / 2, size, size);
    }
    static QRect editRect(const QRect &r, const QFontMetrics &fm) {
        const int width = std::max(88, fm.horizontalAdvance(tr("Edit…")) + 42);
        const int height = std::max(32, fm.height() + 12);
        return QRect(deleteRect(r, fm).left() - width - 10, r.center().y() - height / 2, width, height);
    }
    static QRect checkRect(const QRect &r, const QFontMetrics &fm) {
        const int width = std::max(96, fm.horizontalAdvance(tr("Enabled")) + 34);
        const int height = std::max(28, fm.height() + 8);
        return QRect(editRect(r, fm).left() - width - 10, r.center().y() - height / 2, width, height);
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto rule = index.data(RuleModel::RuleRole).toMap();
        const QRect r = option.rect.adjusted(3, 4, -3, -4);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(selected ? option.palette.highlight().color() : option.palette.mid().color());
        p->setBrush(selected ? option.palette.alternateBase() : option.palette.base());
        p->drawRoundedRect(r.adjusted(0, 0, -1, -1), 4, 4);
        const auto match = rule.value(QStringLiteral("match")).toMap();
        const QString desktop = desktopIdentity(
            match.value(QStringLiteral("desktopFile")).toMap().value(QStringLiteral("value")).toString());
        if (!m_icons.contains(desktop)) {
            const auto service = desktop.isEmpty() ? KService::Ptr()
                                                   : KService::serviceByStorageId(desktop + QStringLiteral(".desktop"));
            const auto name = service ? service->icon() : QStringLiteral("preferences-system-windows");
            auto icon = QDir::isAbsolutePath(name) ? QIcon(name) : QIcon::fromTheme(name);
            if (icon.pixmap(32, 32).isNull())
                icon = QIcon::fromTheme(QStringLiteral("preferences-system-windows"));
            m_icons.insert(desktop, icon);
        }
        m_icons.value(desktop).paint(p, QRect(r.left() + 14, r.center().y() - 18, 36, 36));
        QRect textRect(r.left() + 64, r.top() + 12,
                       std::max(30, checkRect(r, option.fontMetrics).left() - r.left() - 72),
                       option.fontMetrics.height());
        QFont titleFont = option.font;
        titleFont.setBold(true);
        p->setFont(titleFont);
        p->setPen(rule.value(QStringLiteral("enabled")).toBool() ? option.palette.text().color()
                                                                 : option.palette.placeholderText().color());
        p->drawText(textRect, Qt::AlignVCenter,
                    QFontMetrics(titleFont).elidedText(rule.value(QStringLiteral("title")).toString(), Qt::ElideRight,
                                                       textRect.width()));
        p->setFont(option.font);
        p->setPen(option.palette.text().color());
        textRect.translate(0, option.fontMetrics.height() + 5);
        p->drawText(textRect, Qt::AlignVCenter,
                    option.fontMetrics.elidedText(rule.value(QStringLiteral("description")).toString(), Qt::ElideRight,
                                                  textRect.width()));
        textRect.translate(0, option.fontMetrics.height() + 4);
        p->setPen(option.palette.placeholderText().color());
        p->drawText(textRect, Qt::AlignVCenter,
                    option.fontMetrics.elidedText(ruleSummary(rule), Qt::ElideRight, textRect.width()));
        auto style = option.widget ? option.widget->style() : QApplication::style();
        QStyleOptionButton check;
        check.rect = checkRect(r, option.fontMetrics);
        check.palette = option.palette;
        check.text = tr("Enabled");
        check.state = QStyle::State_Enabled |
                      (rule.value(QStringLiteral("enabled")).toBool() ? QStyle::State_On : QStyle::State_Off);
        style->drawControl(QStyle::CE_CheckBox, &check, p, option.widget);
        QStyleOptionButton edit;
        edit.rect = editRect(r, option.fontMetrics);
        edit.palette = option.palette;
        edit.state = QStyle::State_Enabled;
        edit.text = tr("Edit…");
        edit.icon = QIcon::fromTheme(QStringLiteral("document-edit"));
        edit.iconSize = QSize(16, 16);
        style->drawControl(QStyle::CE_PushButton, &edit, p, option.widget);
        QIcon::fromTheme(QStringLiteral("edit-delete"))
            .paint(p, deleteRect(r, option.fontMetrics).adjusted(6, 6, -6, -6));
        p->restore();
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton)
                return false;
            const QRect r = option.rect.adjusted(3, 4, -3, -4);
            const QString id = index.data(RuleModel::RuleRole).toMap().value(QStringLiteral("id")).toString();
            if (checkRect(r, option.fontMetrics).contains(mouse->position().toPoint())) {
                return model->setData(
                    index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                    Qt::CheckStateRole);
            }
            if (editRect(r, option.fontMetrics).contains(mouse->position().toPoint())) {
                Q_EMIT editRequested(id);
                return true;
            }
            if (deleteRect(r, option.fontMetrics).contains(mouse->position().toPoint())) {
                Q_EMIT deleteRequested(id);
                return true;
            }
        }
        if (event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Space || key == Qt::Key_Select) {
                return model->setData(
                    index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                    Qt::CheckStateRole);
            }
        }
        return false;
    }

  private:
    mutable QHash<QString, QIcon> m_icons;
  signals:
    void editRequested(const QString &id);
    void deleteRequested(const QString &id);
};

class RuleDialog : public QDialog {
    Q_OBJECT
  public:
    RuleDialog(const QVariantMap &rule, QWidget *parent) : QDialog(parent), m_rule(rule) {
        setObjectName(QStringLiteral("ruleEditor"));
        setWindowTitle(tr("Edit Rule — Minimize to Tray"));
        setWindowModality(Qt::WindowModal);
        rememberDialog(this, QStringLiteral("rule-editor"), QSize(1000, 900));
        auto dialogLayout = new QVBoxLayout(this);
        auto scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto content = new QWidget;
        auto outer = new QVBoxLayout(content);
        outer->setContentsMargins(14, 12, 14, 12);
        outer->setSpacing(12);
        auto form = new QFormLayout;
        m_title = new QLineEdit(rule.value(QStringLiteral("title")).toString());
        m_title->setMaxLength(256);
        m_title->setObjectName(QStringLiteral("ruleTitle"));
        m_description = new QPlainTextEdit(rule.value(QStringLiteral("description")).toString());
        m_description->setMaximumHeight(72);
        m_description->setObjectName(QStringLiteral("ruleDescription"));
        m_enabled = new QCheckBox(tr("Enable this rule"));
        m_enabled->setObjectName(QStringLiteral("ruleEnabled"));
        m_enabled->setChecked(rule.value(QStringLiteral("enabled")).toBool());
        form->addRow(tr("Name:"), m_title);
        form->addRow(tr("Description:"), m_description);
        form->addRow(QString(), m_enabled);
        outer->addLayout(form);
        auto detectRow = new QHBoxLayout;
        m_detect = makeButton(tr("Detect window properties…"), QStringLiteral("edit-find"));
        m_detect->setObjectName(QStringLiteral("detectWindow"));
        m_delay = new QSpinBox;
        m_delay->setRange(0, 30);
        m_delay->setSuffix(tr(" s"));
        m_delay->setValue(uiState(QStringLiteral("rule-editor")).readEntry("detectDelay", 0));
        connect(this, &QDialog::finished, this, [this]() {
            auto state = uiState(QStringLiteral("rule-editor"));
            state.writeEntry("detectDelay", m_delay->value());
            state.sync();
        });
        m_delay->setAccessibleName(tr("Window detection delay"));
        detectRow->addWidget(m_detect);
        detectRow->addWidget(plainLabel(tr("Delay:")));
        detectRow->addWidget(m_delay);
        detectRow->addStretch();
        outer->addLayout(detectRow);
        outer->addWidget(plainLabel(tr("Select the main window of the app you want to hide.")));
        auto scope = new QHBoxLayout;
        scope->addWidget(plainLabel(tr("Window type:")));
        m_type = new QComboBox;
        m_type->setObjectName(QStringLiteral("windowType"));
        m_type->addItem(tr("Normal application window"), QStringList{QStringLiteral("normal")});
        m_type->addItem(tr("Dialog (non-modal only)"), QStringList{QStringLiteral("dialog")});
        m_type->addItem(tr("Utility window"), QStringList{QStringLiteral("utility")});
        m_type->addItem(tr("Any supported type"),
                        QStringList{QStringLiteral("normal"), QStringLiteral("dialog"), QStringLiteral("utility")});
        m_children = new QCheckBox(tr("Keep child windows and dialogs visible"));
        m_children->setObjectName(QStringLiteral("keepChildrenVisible"));
        const auto match = rule.value(QStringLiteral("match")).toMap();
        m_children->setChecked(!match.value(QStringLiteral("includeChildren")).toBool());
        const auto types = match.value(QStringLiteral("windowTypes")).toStringList();
        int typeIndex = -1;
        for (int i = 0; i < m_type->count(); ++i)
            if (m_type->itemData(i).toStringList() == types)
                typeIndex = i;
        if (typeIndex < 0) {
            m_type->addItem(tr("Selected types: %1").arg(types.join(QStringLiteral(", "))), types);
            typeIndex = m_type->count() - 1;
        }
        m_type->setCurrentIndex(typeIndex);
        scope->addWidget(m_type);
        scope->addStretch();
        scope->addWidget(m_children);
        outer->addLayout(scope);
        auto table = new QGridLayout;
        table->setColumnStretch(3, 1);
        table->addWidget(plainLabel(tr("Use")), 0, 0);
        table->addWidget(plainLabel(tr("Window property")), 0, 1);
        table->addWidget(plainLabel(tr("Match")), 0, 2);
        table->addWidget(plainLabel(tr("Value")), 0, 3);
        int row = 1;
        for (const auto &key : fields) {
            if (key == QLatin1String("legacyClass") && !match.contains(key))
                continue;
            auto use = new QCheckBox;
            use->setObjectName(key + QStringLiteral("Use"));
            use->setAccessibleName(tr("Match %1").arg(fieldLabel(key)));
            auto op = new QComboBox;
            op->setObjectName(key + QStringLiteral("Operator"));
            op->addItem(tr("Equals"), QStringLiteral("equals"));
            if (key == QLatin1String("role") || key == QLatin1String("caption"))
                op->addItem(tr("Contains"), QStringLiteral("contains"));
            auto value = new QLineEdit;
            value->setObjectName(key + QStringLiteral("Value"));
            value->setMaxLength(1024);
            value->setAccessibleName(fieldLabel(key));
            const auto condition = match.value(key).toMap();
            use->setChecked(match.contains(key));
            op->setCurrentIndex(std::max(0, op->findData(condition.value(QStringLiteral("operator")))));
            value->setText(condition.value(QStringLiteral("value")).toString());
            m_conditions.insert(key, {use, op, value});
            table->addWidget(use, row, 0);
            table->addWidget(plainLabel(fieldLabel(key)), row, 1);
            table->addWidget(op, row, 2);
            table->addWidget(value, row++, 3);
            auto updateEnabled = [use, op, value]() {
                op->setEnabled(use->isChecked());
                value->setEnabled(use->isChecked());
            };
            updateEnabled();
            connect(use, &QCheckBox::toggled, this, updateEnabled);
            connect(use, &QCheckBox::toggled, this, &RuleDialog::edited);
            connect(op, &QComboBox::currentIndexChanged, this, &RuleDialog::edited);
            connect(value, &QLineEdit::textChanged, this, &RuleDialog::edited);
            value->setToolTip(tr("Matching is case-sensitive. Application identities use exact matching."));
        }
        outer->addLayout(table);
        outer->addWidget(plainLabel(tr("All selected properties must match. Separate editors may also be Normal "
                                       "windows; use a role or title to distinguish them.")));
        auto previewHeader = new QHBoxLayout;
        auto previewTitle = sectionLabel(tr("Preview with open windows"));
        previewTitle->setWordWrap(false);
        previewHeader->addWidget(previewTitle);
        previewHeader->addStretch();
        auto refresh = makeButton(tr("Refresh"), QStringLiteral("view-refresh"));
        refresh->setObjectName(QStringLiteral("refreshPreview"));
        previewHeader->addWidget(refresh);
        outer->addLayout(previewHeader);
        m_preview = new QTreeWidget;
        m_preview->setObjectName(QStringLiteral("windowPreview"));
        m_preview->setHeaderLabels({tr("Window"), tr("Result")});
        m_preview->setRootIsDecorated(false);
        m_preview->setMinimumHeight(100);
        m_preview->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_preview->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        outer->addWidget(m_preview, 1);
        m_error = plainLabel(QString());
        m_error->hide();
        m_error->setObjectName(QStringLiteral("ruleError"));
        m_error->setForegroundRole(QPalette::Link);
        auto pendingNotice =
            plainLabel(tr("OK updates the rule list. Apply on the configuration page saves your changes."));
        scroll->setWidget(content);
        dialogLayout->addWidget(scroll, 1);
        dialogLayout->addWidget(m_error);
        dialogLayout->addWidget(pendingNotice);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        dialogLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &RuleDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &RuleDialog::reject);
        connect(m_detect, &QPushButton::clicked, this, &RuleDialog::detectWindow);
        connect(refresh, &QPushButton::clicked, this, &RuleDialog::refreshPreview);
        connect(m_type, &QComboBox::currentIndexChanged, this, &RuleDialog::edited);
        connect(m_children, &QCheckBox::toggled, this, &RuleDialog::edited);
        connect(m_enabled, &QCheckBox::toggled, this, &RuleDialog::edited);
        connect(m_title, &QLineEdit::textChanged, this, &RuleDialog::edited);
        connect(m_description, &QPlainTextEdit::textChanged, this, &RuleDialog::edited);
        QTimer::singleShot(0, this, &RuleDialog::refreshPreview);
    }

    QVariantMap rule() const {
        auto result = m_rule;
        result[QStringLiteral("title")] = m_title->text().trimmed();
        result[QStringLiteral("description")] = m_description->toPlainText();
        result[QStringLiteral("enabled")] = m_enabled->isChecked();
        QVariantMap match{{QStringLiteral("windowTypes"), m_type->currentData()},
                          {QStringLiteral("includeChildren"), !m_children->isChecked()}};
        for (auto it = m_conditions.begin(); it != m_conditions.end(); ++it) {
            if (it->use->isChecked())
                match[it.key()] = QVariantMap{{QStringLiteral("operator"), it->op->currentData()},
                                              {QStringLiteral("value"), it->value->text()}};
        }
        result[QStringLiteral("match")] = match;
        return result;
    }

  public slots:
    void accept() override {
        if (m_title->text().trimmed().isEmpty()) {
            setMessage(m_error, tr("Give this rule a name."));
            m_title->setFocus();
            return;
        }
        if (m_description->toPlainText().size() > 4096) {
            setMessage(m_error, tr("The description is too long (maximum 4096 characters)."));
            m_description->setFocus();
            return;
        }
        for (auto it = m_conditions.begin(); it != m_conditions.end(); ++it) {
            if (it->use->isChecked() && it->value->text().trimmed().isEmpty()) {
                setMessage(m_error, tr("Enter a value for %1, or clear its Use checkbox.").arg(fieldLabel(it.key())));
                it->value->setFocus();
                return;
            }
        }
        QVariantList checked;
        QString error;
        if (!decodeRules(encodeRules({rule()}), checked, error)) {
            setMessage(m_error, error);
            return;
        }
        if (m_type->currentData().toStringList() == QStringList{QStringLiteral("dialog")} && m_children->isChecked()) {
            setMessage(m_error, tr("To target a dialog, clear 'Keep child windows and dialogs visible'. Modal dialogs "
                                   "remain protected."));
            return;
        }
        QDialog::accept();
    }

  private slots:
    void detectWindow() {
        if (!m_detect->isEnabled())
            return;
        m_detect->setEnabled(false);
        setMessage(m_error, tr("Select a window with the crosshair. Press Escape to cancel."));
        QTimer::singleShot(m_delay->value() * 1000, this, [this]() {
            hide();
            auto message =
                QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"),
                                               QStringLiteral("org.kde.KWin"), QStringLiteral("queryWindowInfo"));
            auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 120000), this);
            whenFinished(watcher, this, [this](QDBusPendingCallWatcher *watcher) {
                const QDBusPendingReply<QVariantMap> reply = *watcher;
                watcher->deleteLater();
                show();
                raise();
                activateWindow();
                m_detect->setEnabled(true);
                if (reply.isError()) {
                    setMessage(m_error, reply.error().name() == QLatin1String("org.kde.KWin.Error.UserCancel")
                                            ? QString()
                                            : tr("Window detection failed: %1").arg(reply.error().message()));
                    return;
                }
                const auto info = reply.value();
                if (info.isEmpty() || protectedWindow(info)) {
                    setMessage(m_error, tr("Minimize2Tray and its configuration hosts cannot be targeted. Select an "
                                           "application window."));
                    return;
                }
                if (Minimize2Tray::windowType(info) == QLatin1String("other")) {
                    setMessage(m_error, tr("This is a protected system window. Select a normal application window."));
                    return;
                }
                chooseProperties(info);
            });
        });
    }

    void refreshPreview() {
        if (m_previewPending)
            return;
        m_previewPending = true;
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Minimize2Tray"),
                                                      QStringLiteral("com.github.luisbocanegra.Minimize2Tray"),
                                                      QStringLiteral("windows"));
        auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 3000), this);
        whenFinished(watcher, this, [this](QDBusPendingCallWatcher *watcher) {
            const QDBusPendingReply<QVariantList> reply = *watcher;
            watcher->deleteLater();
            m_previewPending = false;
            if (reply.isError()) {
                m_previewNote = tr("Enable Minimize2Tray to preview open windows. Detection still works while the "
                                   "script is disabled.");
            } else {
                m_windows.clear();
                for (const auto &entry : reply.value()) {
                    const auto info = entry.metaType().id() == qMetaTypeId<QDBusArgument>()
                                          ? qdbus_cast<QVariantMap>(entry)
                                          : entry.toMap();
                    if (!info.isEmpty())
                        m_windows.append(info);
                }
                m_previewNote.clear();
            }
            updatePreview();
        });
    }

    void edited() {
        if (!m_preview)
            return;
        setMessage(m_error, QString());
        updatePreview();
    }

    void updatePreview() {
        if (!m_preview)
            return;
        m_preview->clear();
        QVariantList checked;
        QString error;
        const auto candidate = rule();
        if (!decodeRules(encodeRules({candidate}), checked, error)) {
            new QTreeWidgetItem(m_preview, {tr("Choose matching properties"), error});
            return;
        }
        for (const auto &entry : m_windows) {
            const auto info = entry.toMap();
            const QString reason = matchFailure(candidate, info);
            new QTreeWidgetItem(m_preview, {info.value(QStringLiteral("caption")).toString(),
                                            reason.isEmpty() ? tr("Matches") : reason});
        }
        if (!m_previewNote.isEmpty())
            new QTreeWidgetItem(m_preview, {m_previewNote, QString()});
        else if (m_windows.isEmpty())
            new QTreeWidgetItem(m_preview, {tr("No open windows to preview."), QString()});
    }

  private:
    struct Condition {
        QCheckBox *use;
        QComboBox *op;
        QLineEdit *value;
    };
  private slots:
    void chooseProperties(const QVariantMap &selectedInfo) {
        auto info = selectedInfo;
        info[QStringLiteral("desktopFile")] = desktopIdentity(info.value(QStringLiteral("desktopFile")).toString());
        if (info.isEmpty() || protectedWindow(info)) {
            setMessage(m_error, tr("Minimize2Tray and its configuration hosts cannot be targeted."));
            return;
        }
        auto dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("propertyChooser"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setWindowTitle(tr("Detected Window — Minimize to Tray"));
        rememberDialog(dialog, QStringLiteral("detected-properties"), QSize(850, 620));
        auto layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(12);
        layout->addWidget(plainLabel(tr("Use properties from %1").arg(appName(info))));
        layout->addWidget(plainLabel(info.value(QStringLiteral("caption")).toString()));
        layout->addWidget(plainLabel(tr("Choose what identifies the window. These values will fill in the rule.")));
        auto properties = new QTreeWidget;
        properties->setObjectName(QStringLiteral("detectedProperties"));
        properties->setRootIsDecorated(false);
        properties->setHeaderLabels({tr("Use"), tr("Property"), tr("Detected value")});
        bool identitySelected = false;
        for (const auto &key : fields) {
            if (key == QLatin1String("legacyClass") || info.value(key).toString().isEmpty())
                continue;
            auto item = new QTreeWidgetItem(properties, {QString(), fieldLabel(key), info.value(key).toString()});
            item->setData(0, Qt::UserRole, key);
            const bool identity = key == QLatin1String("desktopFile") || key == QLatin1String("resourceClass") ||
                                  key == QLatin1String("resourceName");
            item->setCheckState(0, (!identitySelected && identity) || key == QLatin1String("role") ? Qt::Checked
                                                                                                   : Qt::Unchecked);
            if (identity)
                identitySelected = true;
        }
        auto typeItem =
            new QTreeWidgetItem(properties, {QString(), tr("Window type"), Minimize2Tray::windowType(info)});
        typeItem->setData(0, Qt::UserRole, QStringLiteral("type"));
        typeItem->setCheckState(0, Qt::Checked);
        properties->setColumnWidth(0, 48);
        properties->setColumnWidth(1, 210);
        properties->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        layout->addWidget(properties, 1);
        layout->addWidget(plainLabel(tr(
            "Window roles can distinguish main windows from separate editors. Titles may change as you use the app.")));
        auto children = new QCheckBox(tr("Keep child windows and dialogs visible"));
        children->setChecked(true);
        layout->addWidget(children);
        auto error = plainLabel(QString());
        layout->addWidget(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        auto another = buttons->addButton(tr("Detect another window…"), QDialogButtonBox::ActionRole);
        connect(another, &QPushButton::clicked, dialog, [this, dialog]() {
            dialog->reject();
            detectWindow();
        });
        auto use = buttons->addButton(tr("Use selected properties"), QDialogButtonBox::AcceptRole);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(use, &QPushButton::clicked, dialog, [this, dialog, properties, children, error, info]() {
            QSet<QString> selected;
            for (int i = 0; i < properties->topLevelItemCount(); ++i) {
                auto item = properties->topLevelItem(i);
                if (item->checkState(0) == Qt::Checked)
                    selected.insert(item->data(0, Qt::UserRole).toString());
            }
            if (!selected.contains(QStringLiteral("desktopFile")) &&
                !selected.contains(QStringLiteral("resourceClass")) &&
                !selected.contains(QStringLiteral("resourceName"))) {
                error->setText(tr("Select an application ID, class or instance."));
                return;
            }
            for (auto it = m_conditions.begin(); it != m_conditions.end(); ++it) {
                it->use->setChecked(selected.contains(it.key()));
                it->op->setCurrentIndex(0);
                if (selected.contains(it.key()))
                    it->value->setText(info.value(it.key()).toString());
            }
            int typeIndex = 3;
            if (selected.contains(QStringLiteral("type"))) {
                for (int i = 0; i < m_type->count(); ++i)
                    if (m_type->itemData(i).toStringList() == QStringList{Minimize2Tray::windowType(info)})
                        typeIndex = i;
            }
            m_type->setCurrentIndex(typeIndex);
            m_children->setChecked(children->isChecked());
            if (m_title->text().trimmed().isEmpty() || m_title->text() == tr("New rule"))
                m_title->setText(appName(info));
            if (m_description->toPlainText().trimmed().isEmpty())
                m_description->setPlainText(tr("Automatically hide matching %1 windows.").arg(appName(info)));
            m_windows = {info};
            m_previewNote.clear();
            setMessage(m_error, QString());
            updatePreview();
            dialog->accept();
            refreshPreview();
        });
        dialog->open();
    }

  private:
    QVariantMap m_rule;
    QLineEdit *m_title;
    QPlainTextEdit *m_description;
    QCheckBox *m_enabled;
    QComboBox *m_type;
    QCheckBox *m_children;
    QMap<QString, Condition> m_conditions;
    QPushButton *m_detect;
    QSpinBox *m_delay;
    QTreeWidget *m_preview = nullptr;
    QLabel *m_error;
    QVariantList m_windows;
    QString m_previewNote;
    bool m_previewPending = false;
};
} // namespace

class Minimize2TrayConfig : public KCModule {
    Q_OBJECT
  public:
    Minimize2TrayConfig(QObject *parent, const KPluginMetaData &data)
        : KCModule(parent, data), m_config(QStringLiteral("kwinrc")) {
        setButtons(Apply | Default);
        auto outer = new QVBoxLayout(widget());
        outer->setContentsMargins(16, 16, 16, 16);
        outer->setSpacing(12);
        m_error = plainLabel(QString());
        m_error->hide();
        m_error->setObjectName(QStringLiteral("configError"));
        outer->addWidget(m_error);
        auto reload = makeButton(tr("Reload settings"), QStringLiteral("view-refresh"));
        reload->setObjectName(QStringLiteral("reloadSettings"));
        outer->addWidget(reload);
        reload->hide();
        m_reload = reload;
        connect(reload, &QPushButton::clicked, this, &Minimize2TrayConfig::load);
        m_tabs = new QTabWidget;
        m_tabs->setObjectName(QStringLiteral("configTabs"));
        outer->addWidget(m_tabs, 1);
        auto rulesPage = new QWidget;
        auto rulesLayout = new QVBoxLayout(rulesPage);
        rulesLayout->setContentsMargins(16, 16, 16, 16);
        rulesLayout->setSpacing(12);
        rulesLayout->addWidget(sectionLabel(tr("Windows to hide in the tray")));
        rulesLayout->addWidget(plainLabel(tr("Choose an app window once. Fine-tune which windows are included.")));
        m_search = new QLineEdit;
        m_search->setObjectName(QStringLiteral("ruleSearch"));
        m_search->setClearButtonEnabled(true);
        m_search->setAccessibleName(tr("Search rules"));
        m_search->setPlaceholderText(tr("Search names, descriptions, apps, classes, roles or window titles…"));
        m_search->addAction(QIcon::fromTheme(QStringLiteral("edit-find")), QLineEdit::LeadingPosition);
        rulesLayout->addWidget(m_search);
        auto toolbar = new QHBoxLayout;
        auto add = makeButton(tr("Add rule…"), QStringLiteral("list-add"));
        add->setObjectName(QStringLiteral("addRule"));
        auto text = makeButton(tr("View as text…"), QStringLiteral("document-properties"));
        text->setObjectName(QStringLiteral("viewRulesText"));
        toolbar->addWidget(add);
        toolbar->addWidget(text);
        toolbar->addStretch();
        m_count = plainLabel(QString());
        m_count->setWordWrap(false);
        toolbar->addWidget(m_count);
        rulesLayout->addLayout(toolbar);
        m_model = new RuleModel(this);
        m_proxy = new QSortFilterProxyModel(this);
        m_proxy->setSourceModel(m_model);
        m_proxy->setFilterRole(RuleModel::SearchRole);
        m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        m_list = new QListView;
        m_list->setObjectName(QStringLiteral("ruleList"));
        m_list->setModel(m_proxy);
        m_list->setUniformItemSizes(true);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setMinimumSize(760, 280);
        auto delegate = new RuleDelegate(m_list);
        m_list->setItemDelegate(delegate);
        rulesLayout->addWidget(m_list, 1);
        m_empty = plainLabel(QString());
        m_empty->setObjectName(QStringLiteral("emptyRules"));
        rulesLayout->addWidget(m_empty);
        m_undo = makeButton(tr("Undo delete"), QStringLiteral("edit-undo"));
        m_undo->setObjectName(QStringLiteral("undoDelete"));
        m_undo->hide();
        rulesLayout->addWidget(m_undo);
        rulesLayout->addWidget(
            plainLabel(tr("Disabling or deleting a rule restores windows that no longer match another enabled rule.")));
        m_tabs->addTab(rulesPage, QIcon::fromTheme(QStringLiteral("preferences-system-windows")), tr("Window rules"));
        auto trayPage = new QWidget;
        auto trayLayout = new QVBoxLayout(trayPage);
        m_dot = new QCheckBox(tr("Counter as dot"));
        m_dot->setObjectName(QStringLiteral("countUseDot"));
        trayLayout->addWidget(m_dot);
        auto limitRow = new QFormLayout;
        m_limit = new QSpinBox;
        m_limit->setObjectName(QStringLiteral("countDisplayLimit"));
        m_limit->setRange(1, 9999);
        limitRow->addRow(tr("Counter display limit:"), m_limit);
        trayLayout->addLayout(limitRow);
        trayLayout->addWidget(plainLabel(tr("Larger counts are shown with a plus sign, for example 99+.")));
        m_hide = new QCheckBox(tr("Hide on minimize"));
        m_hide->setObjectName(QStringLiteral("hideOnMinimize"));
        trayLayout->addWidget(m_hide);
        trayLayout->addStretch();
        m_tabs->addTab(trayPage, QIcon::fromTheme(QStringLiteral("preferences-desktop-notification")),
                       tr("Tray && badges"));
        auto advancedPage = new QWidget;
        auto advancedLayout = new QVBoxLayout(advancedPage);
        m_debug = new QCheckBox(tr("Enable debug logging"));
        m_debug->setObjectName(QStringLiteral("debugEnabled"));
        advancedLayout->addWidget(m_debug);
        advancedLayout->addWidget(plainLabel(tr("Minimize2Tray, System Settings, configuration hosts, modal dialogs "
                                                "and system surfaces are protected from hiding.")));
        advancedLayout->addStretch();
        m_tabs->addTab(advancedPage, QIcon::fromTheme(QStringLiteral("configure")), tr("Advanced"));
        outer->addWidget(plainLabel(tr("Changes take effect after Apply or OK.")));
        connect(add, &QPushButton::clicked, this, &Minimize2TrayConfig::addRule);
        connect(text, &QPushButton::clicked, this, &Minimize2TrayConfig::showText);
        connect(m_search, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);
        connect(m_search, &QLineEdit::textChanged, this, &Minimize2TrayConfig::updateCounts);
        connect(m_model, &QAbstractItemModel::dataChanged, this, &Minimize2TrayConfig::changed);
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &Minimize2TrayConfig::changed);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &Minimize2TrayConfig::changed);
        connect(delegate, &RuleDelegate::editRequested, this, &Minimize2TrayConfig::editRule);
        connect(delegate, &RuleDelegate::deleteRequested, this, &Minimize2TrayConfig::deleteRule);
        connect(m_list, &QListView::doubleClicked, this, [this](const QModelIndex &index) {
            editRule(index.data(RuleModel::RuleRole).toMap().value(QStringLiteral("id")).toString());
        });
        auto editAction = new QAction(tr("Edit selected rule…"), m_list);
        editAction->setShortcut(Qt::Key_F2);
        editAction->setShortcutContext(Qt::WidgetShortcut);
        auto deleteAction = new QAction(tr("Delete selected rule"), m_list);
        deleteAction->setShortcut(QKeySequence::Delete);
        deleteAction->setShortcutContext(Qt::WidgetShortcut);
        m_list->addActions({editAction, deleteAction});
        m_list->setContextMenuPolicy(Qt::ActionsContextMenu);
        connect(editAction, &QAction::triggered, this, [this]() { editRule(selectedId()); });
        connect(deleteAction, &QAction::triggered, this, [this]() { deleteRule(selectedId()); });
        connect(m_undo, &QPushButton::clicked, this, [this]() {
            if (m_deleted.isEmpty())
                return;
            if (m_model->rowCount() >= MaxRules) {
                setMessage(m_error,
                           tr("Remove another rule before undoing; the maximum of 1024 rules has been reached."));
                return;
            }
            const auto entry = m_deleted.takeLast();
            m_model->put(entry.second, entry.first);
            m_undo->setVisible(!m_deleted.isEmpty());
        });
        connect(m_dot, &QCheckBox::toggled, this, &Minimize2TrayConfig::changed);
        connect(m_hide, &QCheckBox::toggled, this, &Minimize2TrayConfig::changed);
        connect(m_debug, &QCheckBox::toggled, this, &Minimize2TrayConfig::changed);
        connect(m_limit, &QSpinBox::valueChanged, this, &Minimize2TrayConfig::changed);
        m_stateConnections.append(connect(m_search, &QLineEdit::textChanged, this, [this](const QString &value) {
            if (!m_uiHidden)
                m_savedSearch = value;
        }));
        m_stateConnections.append(connect(m_tabs, &QTabWidget::currentChanged, this, [this](int value) {
            if (!m_uiHidden && value >= 0)
                m_savedTab = value;
        }));
        m_stateConnections.append(connect(
            m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
                if (!m_uiHidden && current.isValid())
                    m_savedSelection = current.data(RuleModel::RuleRole).toMap().value(QStringLiteral("id")).toString();
            }));
        m_stateConnections.append(
            connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
                if (!m_uiHidden)
                    m_savedScroll = value;
            }));
        widget()->installEventFilter(this);
        load();
        const auto state = uiState(QStringLiteral("page"));
        m_search->setText(state.readEntry("search", QString()));
        m_tabs->setCurrentIndex(std::clamp(state.readEntry("tab", 0), 0, 2));
        QTimer::singleShot(0, this,
                           [this, state]() { m_list->verticalScrollBar()->setValue(state.readEntry("scroll", 0)); });
        const int at = m_model->find(state.readEntry("selection", QString()));
        if (at >= 0)
            m_list->setCurrentIndex(m_proxy->mapFromSource(m_model->index(at)));
    }
    ~Minimize2TrayConfig() override {
        for (const auto &connection : m_stateConnections)
            disconnect(connection);
        auto state = uiState(QStringLiteral("page"));
        state.writeEntry("search", m_savedSearch);
        state.writeEntry("tab", m_savedTab);
        state.writeEntry("selection", m_savedSelection);
        state.writeEntry("scroll", m_savedScroll);
        state.sync();
    }

    void load() override {
        m_loading = true;
        const QString previousSelection =
            selectedId().isEmpty() ? uiState(QStringLiteral("page")).readEntry("selection", QString()) : selectedId();
        m_config.reparseConfiguration();
        QString error;
        const auto rules = readRules(m_config, error);
        m_loadError = error;
        m_model->reset(rules);
        const int selected = m_model->find(previousSelection);
        if (selected >= 0)
            m_list->setCurrentIndex(m_proxy->mapFromSource(m_model->index(selected)));
        const auto values = readSettings(m_config);
        m_dot->setChecked(values.value(QStringLiteral("countUseDot")).toBool());
        m_limit->setValue(values.value(QStringLiteral("countDisplayLimit")).toInt());
        m_hide->setChecked(values.value(QStringLiteral("hideOnMinimize")).toBool());
        m_debug->setChecked(values.value(QStringLiteral("debugEnabled")).toBool());
        m_loaded = values;
        m_loaded.insert(QStringLiteral("rules"), KConfigGroup(&m_config, ConfigGroup).readEntry("rules", QString()));
        setMessage(m_error, error);
        m_reload->setVisible(!error.isEmpty());
        m_deleted.clear();
        m_undo->hide();
        m_loading = false;
        updateCounts();
        setNeedsSave(false);
    }
    void save() override {
        QVariantList checked;
        QString error;
        if (!m_loadError.isEmpty() || !decodeRules(encodeRules(m_model->rules), checked, error)) {
            saveError(m_loadError.isEmpty() ? error : m_loadError);
            return;
        }
        QLockFile lock(configLockPath());
        if (!lock.tryLock(0)) {
            saveError(tr("Another configuration window is saving. Try Apply again."));
            return;
        }
        m_config.reparseConfiguration();
        auto current = readSettings(m_config);
        KConfigGroup group(&m_config, ConfigGroup);
        current.insert(QStringLiteral("rules"), group.readEntry("rules", QString()));
        if (current != m_loaded) {
            saveError(tr("Settings changed in another window. Reload before saving; your pending changes have "
                         "not been written."));
            m_reload->show();
            return;
        }
        if (!m_config.isConfigWritable(false)) {
            saveError(tr("The configuration is read-only."));
            return;
        }
        for (const auto *key : {"rules", "countUseDot", "countDisplayLimit", "hideOnMinimize", "debugEnabled"}) {
            if (group.isEntryImmutable(key)) {
                saveError(tr("A setting is locked by the system administrator."));
                return;
            }
        }
        group.writeEntry("rules", encodeRules(m_model->rules));
        group.writeEntry("countUseDot", m_dot->isChecked());
        group.writeEntry("countDisplayLimit", m_limit->value());
        group.writeEntry("hideOnMinimize", m_hide->isChecked());
        group.writeEntry("debugEnabled", m_debug->isChecked());
        if (!m_config.sync()) {
            m_config.markAsClean();
            saveError(tr("Saving failed. Your pending changes are still available; check that kwinrc is writable."));
            return;
        }
        load();
    }
    void defaults() override {
        m_deleted.clear();
        m_undo->hide();
        m_reload->hide();
        m_loadError.clear();
        setMessage(m_error, QString());
        m_loading = true;
        m_model->reset({});
        m_dot->setChecked(false);
        m_limit->setValue(99);
        m_hide->setChecked(false);
        m_debug->setChecked(false);
        m_loading = false;
        changed();
    }

  private slots:
    void addRule() {
        if (m_model->rowCount() >= MaxRules) {
            setMessage(m_error, tr("The maximum of 1024 rules has been reached."));
            return;
        }
        openEditor(newRule());
    }
    void editRule(const QString &id) {
        const int at = m_model->find(id);
        if (at >= 0)
            openEditor(m_model->rules[at].toMap());
    }
    void deleteRule(const QString &id) {
        const int at = m_model->find(id);
        if (at < 0)
            return;
        m_deleted.append({at, m_model->rules[at].toMap()});
        m_model->remove(at);
        m_undo->show();
    }
    void changed() {
        if (m_loading)
            return;
        auto values = readSettingsFromWidgets();
        values.insert(QStringLiteral("rules"), encodeRules(m_model->rules));
        setNeedsSave(values != m_loaded);
        updateCounts();
    }
    void updateCounts() {
        int enabled = 0;
        for (const auto &rule : m_model->rules)
            if (rule.toMap().value(QStringLiteral("enabled")).toBool())
                ++enabled;
        const auto total = m_model->rowCount() == 1 ? tr("1 rule") : tr("%1 rules").arg(m_model->rowCount());
        m_count->setText(tr("%1 · %2 enabled · %3 shown").arg(total).arg(enabled).arg(m_proxy->rowCount()));
        m_empty->setText(m_model->rowCount() == 0
                             ? tr("No rules yet. Choose Add rule to select an app window.")
                             : tr("No matching rules. Try another search or clear the search field."));
        m_empty->setVisible(m_proxy->rowCount() == 0);
        setRepresentsDefaults(m_model->rowCount() == 0 && !m_dot->isChecked() && m_limit->value() == 99 &&
                              !m_hide->isChecked() && !m_debug->isChecked());
    }
    void showText() {
        auto dialog = new QDialog(widget());
        dialog->setObjectName(QStringLiteral("rulesTextDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setWindowTitle(tr("Rules as Text — Minimize to Tray"));
        rememberDialog(dialog, QStringLiteral("rules-text"), QSize(900, 700));
        auto layout = new QVBoxLayout(dialog);
        layout->addWidget(plainLabel(
            m_loadError.isEmpty() ? tr("Read-only JSON. Includes all enabled and disabled rules and any pending edits.")
                                  : tr("Stored rule text could not be parsed. It is shown unchanged for recovery.")));
        auto text = new QPlainTextEdit;
        text->setObjectName(QStringLiteral("rulesJson"));
        text->setReadOnly(true);
        text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setPlainText(m_loadError.isEmpty() ? encodeRules(m_model->rules, true)
                                                 : m_loaded.value(QStringLiteral("rules")).toString());
        layout->addWidget(text, 1);
        auto error = plainLabel(QString());
        layout->addWidget(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        auto copy = buttons->addButton(tr("Copy all"), QDialogButtonBox::ActionRole);
        auto save = buttons->addButton(tr("Save a copy…"), QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(copy, &QPushButton::clicked, dialog,
                [text]() { QApplication::clipboard()->setText(text->toPlainText()); });
        connect(save, &QPushButton::clicked, dialog, [dialog, text, error]() {
            const QString path =
                QFileDialog::getSaveFileName(dialog, tr("Save rules as JSON"),
                                             QStringLiteral("minimize2tray-rules.json"), tr("JSON files (*.json)"));
            if (path.isEmpty())
                return;
            QSaveFile file(path);
            const auto bytes = text->toPlainText().toUtf8();
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                error->setText(tr("Could not save the copy: %1").arg(file.errorString()));
            else
                error->clear();
        });
        dialog->open();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == widget() && event->type() == QEvent::Hide)
            m_uiHidden = true;
        if (watched == widget() && event->type() == QEvent::Show) {
            m_uiHidden = false;
            auto host = widget()->window();
            guardDialog(qobject_cast<QDialog *>(host));
            if (!host->property("minimize2trayStateSaver").toBool()) {
                host->setProperty("minimize2trayStateSaver", true);
                rememberGeometry(host, uiState(QStringLiteral("configuration")));
            }
            restoreGeometryLater(host, uiState(QStringLiteral("configuration")));
        }
        return KCModule::eventFilter(watched, event);
    }

  private:
    void saveError(const QString &message) { setMessage(m_error, message); }
    void guardDialog(QDialog *host) {
        if (!host || host->property("minimize2traySaveGuard").toBool())
            return;
        host->setProperty("minimize2traySaveGuard", true);
        const bool deleteOnClose = host->testAttribute(Qt::WA_DeleteOnClose);
        host->setAttribute(Qt::WA_DeleteOnClose, false);
        // KDE hosts can accept before and after save(); decide after both signals settle.
        connect(host, &QDialog::accepted, host, [host, deleteOnClose]() {
            if (host->property("minimize2traySaveCheck").toBool())
                return;
            host->setProperty("minimize2traySaveCheck", true);
            QTimer::singleShot(0, host, [host, deleteOnClose]() {
                host->setProperty("minimize2traySaveCheck", false);
                bool pending = false;
                for (auto module : host->findChildren<KCModule *>())
                    pending = pending || module->needsSave();
                if (host->result() == QDialog::Accepted && pending)
                    host->show();
                else if (deleteOnClose)
                    host->deleteLater();
            });
        });
        connect(host, &QDialog::rejected, host, [host, deleteOnClose]() {
            if (deleteOnClose)
                host->deleteLater();
        });
    }

    void openEditor(const QVariantMap &rule) {
        auto dialog = new RuleDialog(rule, widget());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::accepted, this, [this, dialog]() { m_model->put(dialog->rule()); });
        dialog->open();
    }
    QString selectedId() const {
        return m_list->currentIndex().data(RuleModel::RuleRole).toMap().value(QStringLiteral("id")).toString();
    }
    QVariantMap readSettingsFromWidgets() const {
        return {{QStringLiteral("countUseDot"), m_dot->isChecked()},
                {QStringLiteral("countDisplayLimit"), m_limit->value()},
                {QStringLiteral("hideOnMinimize"), m_hide->isChecked()},
                {QStringLiteral("debugEnabled"), m_debug->isChecked()}};
    }
    KConfig m_config;
    RuleModel *m_model;
    QSortFilterProxyModel *m_proxy;
    QListView *m_list;
    QLineEdit *m_search;
    QTabWidget *m_tabs;
    QLabel *m_count;
    QLabel *m_error;
    QLabel *m_empty;
    QPushButton *m_reload;
    QPushButton *m_undo;
    QCheckBox *m_dot;
    QSpinBox *m_limit;
    QCheckBox *m_hide;
    QCheckBox *m_debug;
    QVariantMap m_loaded;
    QString m_loadError;
    QList<QPair<int, QVariantMap>> m_deleted;
    bool m_loading = false;
    bool m_uiHidden = false;
    QList<QMetaObject::Connection> m_stateConnections;
    QString m_savedSearch;
    QString m_savedSelection;
    int m_savedTab = 0;
    int m_savedScroll = 0;
};

K_PLUGIN_CLASS_WITH_JSON(Minimize2TrayConfig, "configmodule.json")
#include "configmodule.moc"
