#include "configbackup.h"
#include "rules.h"

#include <KCModule>
#include <KApplicationTrader>
#include <KSycoca>
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
#include <QCollator>
#include <QGuiApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QEventLoopLocker>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLockFile>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <memory>

using namespace Minimize2Tray;

namespace {
template <typename Handler> void whenFinished(QDBusPendingCallWatcher *watcher, QObject *context, Handler handler) {
    // Disconnected or immediately completed calls may finish before a signal can be connected.
    if (watcher->isFinished())
        QTimer::singleShot(0, context, [watcher, handler]() { handler(watcher); });
    else
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, handler);
}

QString toolTipText(const QString &text) {
    return QStringLiteral("<qt>") + text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")) +
           QStringLiteral("</qt>");
}

void comboToolTips(QComboBox *combo, const QStringList &help) {
    for (int i = 0; i < std::min(combo->count(), int(help.size())); ++i)
        combo->setItemData(i, toolTipText(help[i]), Qt::ToolTipRole);
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

QString fieldToolTip(const QString &key) {
    if (key == QLatin1String("desktopFile"))
        return QObject::tr("The application's launcher ID, such as org.kde.konsole, with an optional .desktop suffix. "
                           "Usually the best application identifier. Use Detect to obtain the actual value; matching "
                           "is case-sensitive.");
    if (key == QLatin1String("resourceClass"))
        return QObject::tr("The window class reported by the application. Often shared by all its windows. "
                           "Use Detect to obtain the exact value; matching is case-sensitive.");
    if (key == QLatin1String("resourceName"))
        return QObject::tr("The window instance reported by the application. It can differ from the class, but is "
                           "not necessarily unique to one window. Use Detect; matching is case-sensitive.");
    if (key == QLatin1String("legacyClass"))
        return QObject::tr("An older imported condition that matches either the window class or the window instance "
                           "exactly. Matching is case-sensitive.");
    if (key == QLatin1String("role"))
        return QObject::tr("An application-defined window role, useful for distinguishing a main window from an "
                           "editor or tool window. Some applications do not provide one. Matching is case-sensitive.");
    return QObject::tr("The visible window title. It may change with the open file, page or application state. "
                       "Contains can match a stable part of the title. Matching is case-sensitive.");
}

QString autoCreatedHelp() {
    return QObject::tr("Created by Add tray rule… and kept as generated. Reviewing or accepting it without edits "
                       "keeps this label. Saving changes to the rule removes it. The label itself does not "
                       "change which windows are matched or hidden.");
}

QString ruleDisplayName(const QVariantMap &rule) {
    const auto title = rule.value(QStringLiteral("title")).toString();
    return rule.value(QStringLiteral("autoCreated")).toBool() ? QObject::tr("Auto-created · %1").arg(title) : title;
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
            return ruleDisplayName(rule);
        case Qt::ToolTipRole:
            return searchText(rule) + (rule.value(QStringLiteral("autoCreated")).toBool()
                                           ? QLatin1Char('\n') + autoCreatedHelp()
                                           : QString());
        case Qt::CheckStateRole:
            return rule.value(QStringLiteral("enabled")).toBool() ? Qt::Checked : Qt::Unchecked;
        case Qt::AccessibleTextRole:
            return ruleDisplayName(rule);
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
        const bool enabled = value.toInt() == Qt::Checked;
        if (enabled != rule.value(QStringLiteral("enabled")).toBool())
            rule[QStringLiteral("autoCreated")] = false;
        rule[QStringLiteral("enabled")] = enabled;
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

// Sort the view only. Stored rule order and matching semantics stay independent.
class RuleProxy : public QSortFilterProxyModel {
  public:
    explicit RuleProxy(QObject *parent) : QSortFilterProxyModel(parent) {
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
        m_collator.setNumericMode(true);
        sort(0);
    }
    void setOrder(const QString &order) {
        m_order = order;
        invalidate();
    }
  protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override {
        const auto a = left.data(RuleModel::RuleRole).toMap();
        const auto b = right.data(RuleModel::RuleRole).toMap();
        if (m_order != QLatin1String("title")) {
            const auto ad = a.value(QStringLiteral("createdAt")).toString();
            const auto bd = b.value(QStringLiteral("createdAt")).toString();
            if (ad.isEmpty() != bd.isEmpty())
                return !ad.isEmpty(); // Unknown dates always follow known dates.
            if (ad.isEmpty())
                return left.row() < right.row(); // Preserve older rules' stored order.
            if (ad != bd)
                return m_order == QLatin1String("oldest") ? ad < bd : ad > bd;
        }
        const int title = m_collator.compare(a.value(QStringLiteral("title")).toString(),
                                            b.value(QStringLiteral("title")).toString());
        return title ? title < 0 : a.value(QStringLiteral("id")).toString() < b.value(QStringLiteral("id")).toString();
    }
  private:
    QCollator m_collator;
    QString m_order = QStringLiteral("title");
};

class RuleDelegate : public QStyledItemDelegate {
    Q_OBJECT
  public:
    explicit RuleDelegate(QObject *parent) : QStyledItemDelegate(parent) {
        connect(KSycoca::self(), &KSycoca::databaseChanged, this, [this]() {
            m_icons.clear();
            m_windowClasses.clear();
            m_foldedIdentities.clear();
            m_windowClassesReady = false;
            if (auto view = qobject_cast<QAbstractItemView *>(this->parent()))
                view->viewport()->update();
        }, Qt::QueuedConnection);
    }
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
        ruleIcon(rule.value(QStringLiteral("match")).toMap())
            .paint(p, QRect(r.left() + 14, r.center().y() - 18, 36, 36));
        QRect textRect(r.left() + 64, r.top() + 12,
                       std::max(30, checkRect(r, option.fontMetrics).left() - r.left() - 72),
                       option.fontMetrics.height());
        QFont titleFont = option.font;
        titleFont.setBold(true);
        p->setFont(titleFont);
        p->setPen(rule.value(QStringLiteral("enabled")).toBool() ? option.palette.text().color()
                                                                 : option.palette.placeholderText().color());
        p->drawText(textRect, Qt::AlignVCenter,
                    QFontMetrics(titleFont).elidedText(ruleDisplayName(rule), Qt::ElideRight, textRect.width()));
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
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
        if (!event || !view || !index.isValid())
            return false;
        if (event->type() != QEvent::ToolTip)
            return QStyledItemDelegate::helpEvent(event, view, option, index);
        const auto rect = option.rect.adjusted(3, 4, -3, -4);
        QString help;
        if (checkRect(rect, option.fontMetrics).contains(event->pos())) {
            help = tr("Enable or disable this rule. Changes take effect after Apply or OK.");
            if (index.data(RuleModel::RuleRole).toMap().value(QStringLiteral("autoCreated")).toBool())
                help += QLatin1Char('\n') + tr("Saving this change removes the Automatically created label.");
        } else if (editRect(rect, option.fontMetrics).contains(event->pos()))
            help = tr("Edit this rule's matching properties and actions.");
        else if (deleteRect(rect, option.fontMetrics).contains(event->pos()))
            help = tr("Delete this rule. Undo delete restores it before Apply or OK.");
        else
            help = index.data(Qt::ToolTipRole).toString();
        // Imported descriptions and titles are plain text, never tooltip HTML.
        QToolTip::showText(event->globalPos(), toolTipText(help), view->viewport());
        return true;
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
    QIcon ruleIcon(const QVariantMap &match) const {
        QStringList identities;
        for (const auto *key : {"desktopFile", "resourceClass", "resourceName", "legacyClass"})
            identities.append(match.value(QLatin1String(key)).toMap().value(QStringLiteral("value")).toString());
        // Include every identity; class-only rules must not share the empty desktop-ID cache entry.
        const auto cacheKey = identities.join(QChar::Null);
        const auto cached = m_icons.constFind(cacheKey);
        if (cached != m_icons.cend()) return cached.value();
        QIcon icon;
        for (const auto &identity : identities) {
            // Only installed identities are allowed, never window/rule-supplied desktop paths.
            if (identity.isEmpty() || identity.contains(QLatin1Char('/')) || identity.contains(QLatin1Char('\\')) ||
                identity.contains(QLatin1Char(':')) || identity.contains(QChar::Null)) continue;
            const auto desktop = identity.endsWith(QLatin1String(".desktop"))
                                     ? identity : identity + QStringLiteral(".desktop");
            auto service = KService::serviceByStorageId(desktop);
            if (!service) {
                if (!m_windowClassesReady) {
                    // Build the metadata alias index once, not on every row repaint.
                    const auto addAlias = [](auto &index, const QString &alias, const KService::Ptr &application) {
                        if (alias.isEmpty()) return;
                        if (!index.contains(alias))
                            index.insert(alias, application);
                        else if (const auto previous = index.value(alias);
                                 previous && previous->storageId() != application->storageId())
                            index[alias] = {}; // Never choose a random application for an ambiguous alias.
                    };
                    for (const auto &application : KApplicationTrader::query([](const KService::Ptr &) { return true; })) {
                        const auto wmClass = application->property<QString>(QStringLiteral("StartupWMClass"));
                        addAlias(m_windowClasses, wmClass, application);
                        for (const auto &alias : {wmClass, application->desktopEntryName(),
                                                 desktopIdentity(application->storageId())})
                            addAlias(m_foldedIdentities, alias.toCaseFolded(), application);
                    }
                    m_windowClassesReady = true;
                }
                service = m_windowClasses.value(identity);
                // Presentation-only fallback for legacy class capitalization. Rule
                // matching remains case-sensitive, and only unique aliases are used.
                if (!service && !m_windowClasses.contains(identity))
                    service = m_foldedIdentities.value(desktopIdentity(identity).toCaseFolded());
            }
            if (!service || !service->isApplication()) continue;
            const auto name = service->icon();
            const auto candidate = QDir::isAbsolutePath(name) ? QIcon(name) : QIcon::fromTheme(name);
            if (!candidate.pixmap(32, 32).isNull()) {
                icon = candidate;
                break;
            }
        }
        if (icon.isNull()) icon = QIcon::fromTheme(QStringLiteral("preferences-system-windows"));
        m_icons.insert(cacheKey, icon);
        return icon;
    }
    mutable QHash<QString, QIcon> m_icons;
    mutable QHash<QString, KService::Ptr> m_windowClasses;
    mutable QHash<QString, KService::Ptr> m_foldedIdentities;
    mutable bool m_windowClassesReady = false;
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
        m_autoCreatedNotice = sectionLabel(tr("Automatically created"));
        m_autoCreatedNotice->setObjectName(QStringLiteral("autoCreatedRuleNotice"));
        m_autoCreatedNotice->setToolTip(toolTipText(autoCreatedHelp()));
        outer->addWidget(m_autoCreatedNotice);
        m_autoCreatedNotice->setVisible(rule.value(QStringLiteral("autoCreated")).toBool());
        auto form = new QFormLayout;
        m_title = new QLineEdit(rule.value(QStringLiteral("title")).toString());
        m_title->setMaxLength(256);
        m_title->setObjectName(QStringLiteral("ruleTitle"));
        m_title->setToolTip(
            toolTipText(tr("A name for this rule in the list. This does not match the application’s window title.")));
        m_description = new QPlainTextEdit(rule.value(QStringLiteral("description")).toString());
        m_description->setMaximumHeight(72);
        m_description->setObjectName(QStringLiteral("ruleDescription"));
        m_description->setToolTip(
            toolTipText(tr("An optional note about this rule. It does not affect matching or actions.")));
        m_enabled = new QCheckBox(tr("Enable this rule"));
        m_enabled->setObjectName(QStringLiteral("ruleEnabled"));
        m_enabled->setToolTip(
            toolTipText(tr("Apply this rule to matching windows. Disable it to keep the rule for later without "
                           "applying its actions.")));
        m_enabled->setChecked(rule.value(QStringLiteral("enabled")).toBool());
        form->addRow(tr("Name:"), m_title);
        form->addRow(tr("Description:"), m_description);
        form->addRow(QString(), m_enabled);
        outer->addLayout(form);
        const auto behavior = rule.value(QStringLiteral("behavior")).toMap();
        auto actions = new QFormLayout;
        m_startup = new QComboBox;
        m_startup->setObjectName(QStringLiteral("startupAction"));
        m_startup->setToolTip(toolTipText(tr(
            "Choose what happens when a window first matches: leave it alone, add a tray icon while keeping its "
            "current visibility, or hide it in the tray. Also applies to existing windows when rules are applied.")));
        m_startup->addItem(tr("No startup action"), QStringLiteral("none"));
        m_startup->addItem(tr("Pin without initially hiding"), QStringLiteral("pin"));
        m_startup->addItem(tr("Start in tray"), QStringLiteral("hide"));
        m_startup->setCurrentIndex(behavior.value(QStringLiteral("startInTray")).toBool() ? 2
                                   : behavior.value(QStringLiteral("autoPin")).toBool()   ? 1
                                                                                          : 0);
        comboToolTips(m_startup, {tr("Leave the window as it is when the rule first matches."),
                                  tr("Add a tray icon without changing whether the window is visible or minimized."),
                                  tr("Create a tray icon and hide the window on first match, including existing "
                                     "windows when applying rules.")});
        actions->addRow(tr("Startup:"), m_startup);
        m_startupProtection = new QCheckBox(tr("Retry early startup restoration once"));
        m_startupProtection->setObjectName(QStringLiteral("startupProtection"));
        m_startupProtection->setChecked(behavior.value(QStringLiteral("startupProtection")).toBool());
        m_startupProtection->setEnabled(m_startup->currentData() == QLatin1String("hide"));
        m_startupProtection->setToolTip(
            toolTipText(tr("If an app immediately reopens its window after Start in tray, hide it once "
                           "more within 1.5 seconds unless it becomes active. Show, Unpin and "
                           "activation cancel the retry. A user restore without activation can also "
                           "be retried; leave this off if it interferes.")));
        actions->addRow(QString(), m_startupProtection);
        m_minimize = new QComboBox;
        m_minimize->setObjectName(QStringLiteral("minimizeToTray"));
        m_minimize->setToolTip(
            toolTipText(tr("On hides matching windows in the tray when minimized. Off keeps ordinary "
                           "minimization. Use global follows Hide on minimize on the Tray & badges page.")));
        m_minimize->addItem(tr("Use global Hide on minimize setting"), QStringLiteral("inherit"));
        m_minimize->addItem(tr("On"), QStringLiteral("enabled"));
        m_minimize->addItem(tr("Off"), QStringLiteral("disabled"));
        m_minimize->setCurrentIndex(m_minimize->findData(behavior.value(QStringLiteral("minimizeToTray"))));
        comboToolTips(m_minimize,
                      {tr("Follow Hide on minimize on the Tray & badges page."),
                       tr("Hide the window in the tray when it is minimized."),
                       tr("Allow ordinary minimization without tray hiding, even if the window has a tray icon.")});
        actions->addRow(tr("Minimize to tray:"), m_minimize);
        m_close = new QCheckBox(tr("Close to tray through the optional shortcut"));
        m_close->setObjectName(QStringLiteral("closeToTray"));
        m_close->setToolTip(
            toolTipText(tr("Hide the matching window when you use the separately assigned ‘Close or hide window to "
                           "tray’ shortcut. This does not intercept titlebar Close, Alt+F4 or application Quit.")));
        m_close->setChecked(behavior.value(QStringLiteral("closeToTray")).toBool());
        actions->addRow(QString(), m_close);
        m_quitOnLogout = new QCheckBox(tr("Quit on logout"));
        m_quitOnLogout->setObjectName(QStringLiteral("terminateOnLogout"));
        m_quitOnLogout->setChecked(behavior.value(QStringLiteral("terminateOnLogout")).toBool());
        m_quitOnLogout->setToolTip(
            toolTipText(tr("During logout, restart or shutdown, send SIGTERM if KDE has disabled closing "
                           "for a matching window. This terminates its whole process, possibly without "
                           "save prompts. Requires a verified native Wayland process, Linux 6.13+ and "
                           "an adapter built for the running KWin. XWayland and protected processes are "
                           "left alone. Cancelling logout cannot restore a terminated application.")));
        actions->addRow(QString(), m_quitOnLogout);
#if !MINIMIZE2TRAY_HAS_SESSION_PROCESS
        // Preserve imported settings, but still allow disabling an existing opt-in.
        m_quitOnLogout->setEnabled(m_quitOnLogout->isChecked());
        connect(m_quitOnLogout, &QCheckBox::toggled, this, [this](bool enabled) {
            if (!enabled)
                m_quitOnLogout->setEnabled(false);
        });
        actions->addRow(QString(),
                        plainLabel(tr("Quit on logout is unavailable: this build has no matching KWin adapter.")));
#endif
        outer->addLayout(actions);
        outer->addWidget(plainLabel(tr("Minimize and close actions do not require a startup action. Start in tray "
                                       "hides on first match, including existing windows when applying rules. "
                                       "Overlapping rules combine their enabled actions; explicit Minimize Off "
                                       "overrides the global setting unless another matching rule enables it.")));
        outer->addWidget(plainLabel(tr("Close to tray requires assigning the separate ‘Close or hide window to tray’ "
                                       "shortcut in System Settings. It has no default binding. Titlebar Close, "
                                       "application Quit and the tray Quit action still close normally.")));
        auto detectRow = new QHBoxLayout;
        m_detect = makeButton(tr("Detect window properties…"), QStringLiteral("edit-find"));
        m_detect->setObjectName(QStringLiteral("detectWindow"));
        m_detect->setToolTip(toolTipText(
            tr("Read a window’s properties with KWin’s picker, then choose which values to copy into this "
               "editor. Your selection can replace existing matching fields. Nothing is saved until "
               "you confirm this editor and choose Apply or OK in configuration. Escape cancels picking.")));
        m_delay = new QSpinBox;
        m_delay->setObjectName(QStringLiteral("detectDelay"));
        m_delay->setToolTip(toolTipText(tr(
            "Wait this many seconds before starting the window picker, giving you time to open the desired window.")));
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
        m_suggestionNotice =
            plainLabel(tr("Suggested values are shown in color. They do not affect the rule unless "
                          "you select Use for that row and save. Existing fields and actions are unchanged."));
        m_suggestionNotice->setObjectName(QStringLiteral("ruleSuggestionNotice"));
        m_suggestionNotice->hide();
        outer->addWidget(m_suggestionNotice);
        outer->addWidget(plainLabel(tr("Select the main window of the app you want to pin.")));
        auto scope = new QHBoxLayout;
        scope->addWidget(plainLabel(tr("Window type:")));
        m_type = new QComboBox;
        m_type->setObjectName(QStringLiteral("windowType"));
        m_type->setToolTip(
            toolTipText(tr("Limit this rule to normal windows, utility windows or non-modal dialogs. Modal dialogs "
                           "and system windows remain protected.")));
        m_type->addItem(tr("Normal application window"), QStringList{QStringLiteral("normal")});
        m_type->addItem(tr("Dialog (non-modal only)"), QStringList{QStringLiteral("dialog")});
        m_type->addItem(tr("Utility window"), QStringList{QStringLiteral("utility")});
        m_type->addItem(tr("Any supported type"),
                        QStringList{QStringLiteral("normal"), QStringLiteral("dialog"), QStringLiteral("utility")});
        comboToolTips(m_type, {tr("Match normal application windows."),
                               tr("Match non-modal dialogs, subject to child-window protection."),
                               tr("Match utility or tool windows, subject to child-window protection."),
                               tr("Match any supported normal, non-modal dialog or utility window.")});
        m_children = new QCheckBox(tr("Keep child windows and dialogs visible"));
        m_children->setObjectName(QStringLiteral("keepChildrenVisible"));
        m_children->setToolTip(
            toolTipText(tr("Keep checked to exclude child windows and dialogs. Unchecking allows matching "
                           "non-modal children of the selected types; modal dialogs remain protected.")));
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
            m_type->setItemData(typeIndex, toolTipText(tr("Match only the window types listed in this choice.")),
                                Qt::ToolTipRole);
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
            use->setToolTip(
                toolTipText(tr("Require this property to match. All checked properties must match together.") +
                            QLatin1Char('\n') + fieldToolTip(key)));
            auto op = new QComboBox;
            op->setObjectName(key + QStringLiteral("Operator"));
            op->setToolTip(
                toolTipText(key == QLatin1String("role") || key == QLatin1String("caption")
                                ? tr("Equals matches the complete value; Contains matches a literal part of it. Both "
                                     "are case-sensitive; wildcards and regular expressions are not supported.")
                                : tr("Match the complete application identifier exactly, including capitalization.")));
            op->addItem(tr("Equals"), QStringLiteral("equals"));
            if (key == QLatin1String("role") || key == QLatin1String("caption"))
                op->addItem(tr("Contains"), QStringLiteral("contains"));
            comboToolTips(op, {tr("Match the entire value, including capitalization."),
                               tr("Match a literal part of the value, including capitalization. No wildcards or "
                                  "regular expressions.")});
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
            auto label = plainLabel(fieldLabel(key));
            label->setToolTip(toolTipText(fieldToolTip(key)));
            table->addWidget(label, row, 1);
            table->addWidget(op, row, 2);
            table->addWidget(value, row++, 3);
            auto updateEnabled = [use, op, value]() {
                op->setEnabled(use->isChecked());
                value->setEnabled(use->isChecked() || value->property("minimize2traySuggestion").toBool());
            };
            updateEnabled();
            connect(use, &QCheckBox::toggled, this, updateEnabled);
            connect(use, &QCheckBox::toggled, this,
                    [use, value, key, help = use->toolTip(), palette = value->palette()](bool checked) {
                        if (checked && value->property("minimize2traySuggestion").toBool()) {
                            value->setProperty("minimize2traySuggestion", false);
                            value->setPalette(palette);
                            value->setToolTip(toolTipText(fieldToolTip(key)));
                            use->setToolTip(help);
                        }
                    });
            connect(use, &QCheckBox::toggled, this, &RuleDialog::edited);
            connect(op, &QComboBox::currentIndexChanged, this, &RuleDialog::edited);
            connect(value, &QLineEdit::textChanged, this, &RuleDialog::edited);
            value->setToolTip(toolTipText(fieldToolTip(key)));
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
        refresh->setToolTip(
            toolTipText(tr("Refresh the list of open windows and check them against your unsaved rule edits.")));
        previewHeader->addWidget(refresh);
        outer->addLayout(previewHeader);
        m_preview = new QTreeWidget;
        m_preview->setObjectName(QStringLiteral("windowPreview"));
        m_preview->setToolTip(toolTipText(
            tr("Shows which open windows match this rule and why others are excluded. Uses your unsaved edits.")));
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
        buttons->button(QDialogButtonBox::Ok)
            ->setToolTip(
                toolTipText(tr("Update the pending rule list with these edits. Apply or OK on the main page saves the "
                               "configuration.")));
        buttons->button(QDialogButtonBox::Cancel)
            ->setToolTip(toolTipText(tr("Discard changes made in this rule editor.")));
        dialogLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &RuleDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &RuleDialog::reject);
        connect(m_detect, &QPushButton::clicked, this, &RuleDialog::detectWindow);
        connect(refresh, &QPushButton::clicked, this, &RuleDialog::refreshPreview);
        connect(m_type, &QComboBox::currentIndexChanged, this, &RuleDialog::edited);
        connect(m_children, &QCheckBox::toggled, this, &RuleDialog::edited);
        connect(m_enabled, &QCheckBox::toggled, this, &RuleDialog::edited);
        connect(m_startup, &QComboBox::currentIndexChanged, this, [this]() {
            m_startupProtection->setEnabled(m_startup->currentData() == QLatin1String("hide"));
            edited();
        });
        connect(m_startupProtection, &QCheckBox::toggled, this, &RuleDialog::edited);
        connect(m_minimize, &QComboBox::currentIndexChanged, this, &RuleDialog::edited);
        connect(m_close, &QCheckBox::toggled, this, &RuleDialog::edited);
        connect(m_quitOnLogout, &QCheckBox::toggled, this, &RuleDialog::edited);
        connect(m_title, &QLineEdit::textChanged, this, &RuleDialog::edited);
        connect(m_description, &QPlainTextEdit::textChanged, this, &RuleDialog::edited);
        // Compare final editor contents, not whether a control was clicked.
        // Accepting or undoing edits back to the generated values keeps the flag.
        m_initialRule = this->rule();
        QTimer::singleShot(0, this, &RuleDialog::refreshPreview);
    }

    void suggestProperties(const QVariantMap &info) {
        bool suggested = false;
        for (auto it = m_conditions.begin(); it != m_conditions.end(); ++it) {
            // An unchecked field can still contain a user's pending text.
            if (it->use->isChecked() || !it->value->text().isEmpty())
                continue;
            auto value = info.value(it.key()).toString();
            if (it.key() == QLatin1String("desktopFile"))
                value = desktopIdentity(value);
            if (it.key() == QLatin1String("legacyClass") || value.isEmpty() || value.size() > 1024)
                continue;
            it->value->setProperty("minimize2traySuggestion", true);
            it->value->setText(value);
            it->value->setEnabled(true);
            auto palette = it->value->palette();
            palette.setColor(QPalette::Text, palette.color(QPalette::Link));
            it->value->setPalette(palette);
            const auto help = toolTipText(tr("Suggested value read from the selected window. It is not used unless "
                                             "you select Use for this row and save the rule. Your existing "
                                             "matching fields and actions have not been changed.") +
                                          QLatin1Char('\n') + fieldToolTip(it.key()));
            it->value->setToolTip(help);
            it->use->setToolTip(help);
            suggested = true;
        }
        m_suggestionNotice->setVisible(suggested);
    }

    QVariantMap rule() const {
        auto result = m_rule;
        result[QStringLiteral("title")] = m_title->text().trimmed();
        result[QStringLiteral("description")] = m_description->toPlainText();
        result[QStringLiteral("enabled")] = m_enabled->isChecked();
        result[QStringLiteral("behavior")] =
            QVariantMap{{QStringLiteral("autoPin"), m_startup->currentData() == QLatin1String("pin")},
                        {QStringLiteral("startInTray"), m_startup->currentData() == QLatin1String("hide")},
                        {QStringLiteral("minimizeToTray"), m_minimize->currentData()},
                        {QStringLiteral("closeToTray"), m_close->isChecked()},
                        {QStringLiteral("terminateOnLogout"), m_quitOnLogout->isChecked()},
                        {QStringLiteral("startupProtection"),
                         m_startup->currentData() == QLatin1String("hide") && m_startupProtection->isChecked()}};
        QVariantMap match{{QStringLiteral("windowTypes"), m_type->currentData()},
                          {QStringLiteral("includeChildren"), !m_children->isChecked()}};
        for (auto it = m_conditions.begin(); it != m_conditions.end(); ++it) {
            if (it->use->isChecked())
                match[it.key()] = QVariantMap{{QStringLiteral("operator"), it->op->currentData()},
                                              {QStringLiteral("value"), it->value->text()}};
        }
        result[QStringLiteral("match")] = match;
        if (!m_initialRule.isEmpty() && result != m_initialRule)
            result[QStringLiteral("autoCreated")] = false;
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
        m_autoCreatedNotice->setVisible(rule().value(QStringLiteral("autoCreated")).toBool());
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
        properties->setToolTip(toolTipText(tr("Select at least one application identifier and any optional role, title or type. All selected properties must match together.")));
        properties->setRootIsDecorated(false);
        properties->setHeaderLabels({tr("Use"), tr("Property"), tr("Detected value")});
        bool identitySelected = false;
        for (const auto &key : fields) {
            if (key == QLatin1String("legacyClass") || info.value(key).toString().isEmpty())
                continue;
            auto item = new QTreeWidgetItem(properties, {QString(), fieldLabel(key), info.value(key).toString()});
            item->setData(0, Qt::UserRole, key);
            for (int column = 0; column < 3; ++column)
                item->setToolTip(column, toolTipText(fieldToolTip(key)));
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
        for (int column = 0; column < 3; ++column)
            typeItem->setToolTip(
                column,
                toolTipText(tr("Require the detected window type, such as a normal window or non-modal dialog.")));
        properties->setColumnWidth(0, 48);
        properties->setColumnWidth(1, 210);
        properties->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        layout->addWidget(properties, 1);
        layout->addWidget(plainLabel(tr(
            "Window roles can distinguish main windows from separate editors. Titles may change as you use the app.")));
        auto children = new QCheckBox(tr("Keep child windows and dialogs visible"));
        children->setObjectName(QStringLiteral("detectedKeepChildrenVisible"));
        children->setToolTip(
            toolTipText(tr("Exclude child windows and dialogs from this rule. Modal dialogs remain protected even "
                           "when this is unchecked.")));
        children->setChecked(true);
        layout->addWidget(children);
        auto error = plainLabel(QString());
        layout->addWidget(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Cancel)
            ->setToolTip(toolTipText(tr("Keep the rule’s current properties and close this detection result.")));
        auto another = buttons->addButton(tr("Detect another window…"), QDialogButtonBox::ActionRole);
        another->setToolTip(toolTipText(tr("Pick a different window instead of using these detected properties.")));
        connect(another, &QPushButton::clicked, dialog, [this, dialog]() {
            dialog->reject();
            detectWindow();
        });
        auto use = buttons->addButton(tr("Use selected properties"), QDialogButtonBox::AcceptRole);
        use->setToolTip(
            toolTipText(tr("Copy the checked properties into the rule editor. Review and save the rule there.")));
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
    QVariantMap m_initialRule;
    QLabel *m_autoCreatedNotice;
    QLabel *m_suggestionNotice;
    QLineEdit *m_title;
    QPlainTextEdit *m_description;
    QCheckBox *m_enabled;
    QComboBox *m_startup;
    QCheckBox *m_startupProtection;
    QComboBox *m_minimize;
    QCheckBox *m_close;
    QCheckBox *m_quitOnLogout;
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
    Q_CLASSINFO("D-Bus Interface", "com.github.luisbocanegra.Minimize2Tray.Configuration")
  public:
    Minimize2TrayConfig(QObject *parent, const KPluginMetaData &data, const QVariantList &args)
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
        reload->setToolTip(
            toolTipText(tr("Reload saved rules and options, discarding pending edits in this configuration window.")));
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
        rulesLayout->addWidget(sectionLabel(tr("Windows to pin in the tray")));
        rulesLayout->addWidget(plainLabel(tr("Choose an app window once. Fine-tune which windows are included.")));
        m_search = new QLineEdit;
        m_search->setObjectName(QStringLiteral("ruleSearch"));
        m_search->setToolTip(
            toolTipText(tr("Filter the list by rule names, descriptions or matching properties. Filtering does "
                           "not change which rules are enabled.")));
        m_search->setClearButtonEnabled(true);
        m_search->setAccessibleName(tr("Search rules"));
        m_search->setPlaceholderText(tr("Search names, descriptions, apps, classes, roles or window titles…"));
        m_search->addAction(QIcon::fromTheme(QStringLiteral("edit-find")), QLineEdit::LeadingPosition);
        rulesLayout->addWidget(m_search);
        auto toolbar = new QHBoxLayout;
        auto add = makeButton(tr("Add rule…"), QStringLiteral("list-add"));
        add->setObjectName(QStringLiteral("addRule"));
        add->setToolTip(toolTipText(
            tr("Open a new rule editor. Use Detect to fill window properties, then choose the actions. "
               "Confirm the editor and choose Apply or OK in configuration to save. To generate and "
               "save a rule directly from a picked window, use Add tray rule… from a tray icon or its shortcut.")));
        auto text = makeButton(tr("View as text…"), QStringLiteral("document-properties"));
        text->setObjectName(QStringLiteral("viewRulesText"));
        text->setToolTip(
            toolTipText(tr("Inspect, copy or save rule JSON, including disabled rules and pending edits. Full "
                           "configuration export is also available there.")));
        toolbar->addWidget(add);
        toolbar->addWidget(text);
        toolbar->addStretch();
        auto sortLabel = new QLabel(tr("Sort by:"));
        m_sort = new QComboBox;
        m_sort->setObjectName(QStringLiteral("ruleSort"));
        m_sort->addItem(tr("Title (A–Z)"), QStringLiteral("title"));
        m_sort->addItem(tr("Created (newest first)"), QStringLiteral("newest"));
        m_sort->addItem(tr("Created (oldest first)"), QStringLiteral("oldest"));
        m_sort->setToolTip(
            toolTipText(tr("Choose title or creation-time order for the list. The choice is remembered between "
                           "sessions. Older rules with unknown dates follow dated rules in their stored order.")));
        sortLabel->setBuddy(m_sort);
        toolbar->addWidget(sortLabel);
        toolbar->addWidget(m_sort);
        comboToolTips(m_sort,
                      {tr("Sort rule names alphabetically, using natural number order."),
                       tr("Show the most recently created rules first. Rules with unknown dates follow dated rules."),
                       tr("Show the earliest created rules first. Rules with unknown dates follow dated rules.")});
        m_count = plainLabel(QString());
        m_count->setWordWrap(false);
        toolbar->addWidget(m_count);
        rulesLayout->addLayout(toolbar);
        m_model = new RuleModel(this);
        m_proxy = new RuleProxy(this);
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
        m_undo->setToolTip(toolTipText(
            tr("Restore the most recently deleted rule. Available until Apply or OK saves the deletions.")));
        m_undo->hide();
        rulesLayout->addWidget(m_undo);
        rulesLayout->addWidget(
            plainLabel(tr("Disabling or deleting a rule removes automatic pins when no other enabled rule matches, "
                          "restoring windows hidden in the tray.")));
        m_tabs->addTab(rulesPage, QIcon::fromTheme(QStringLiteral("preferences-system-windows")), tr("Window rules"));
        auto trayPage = new QWidget;
        auto trayLayout = new QVBoxLayout(trayPage);
        m_dot = new QCheckBox(tr("Counter as dot"));
        m_dot->setObjectName(QStringLiteral("countUseDot"));
        m_dot->setToolTip(
            toolTipText(tr("Show a dot instead of a number when an application publishes a positive Unity counter. "
                           "Notification activity does not supply these counts.")));
        trayLayout->addWidget(m_dot);
        auto limitRow = new QFormLayout;
        m_limit = new QSpinBox;
        m_limit->setObjectName(QStringLiteral("countDisplayLimit"));
        m_limit->setToolTip(
            toolTipText(tr("Largest counter number to display. Higher counts use a plus sign, such as 99+. This "
                           "changes the display, not the application’s count.")));
        m_limit->setRange(1, 9999);
        limitRow->addRow(tr("Counter display limit:"), m_limit);
        trayLayout->addLayout(limitRow);
        trayLayout->addWidget(plainLabel(tr("Larger counts are shown with a plus sign, for example 99+.")));
        m_hide = new QCheckBox(tr("Hide on minimize"));
        m_hide->setObjectName(QStringLiteral("hideOnMinimize"));
        m_hide->setToolTip(
            toolTipText(tr("Default minimize behaviour for rules using the global setting and manually pinned "
                           "windows without a matching rule. A rule’s On or Off setting overrides this default.")));
        trayLayout->addWidget(m_hide);
        trayLayout->addWidget(
            plainLabel(tr("Default for rules using ‘Use global setting’ and for manually pinned windows "
                          "without a matching rule. Per-rule On or Off overrides this default.")));
        trayLayout->addStretch();
        m_tabs->addTab(trayPage, QIcon::fromTheme(QStringLiteral("preferences-desktop-notification")),
                       tr("Tray && badges"));
        auto advancedPage = new QWidget;
        auto advancedLayout = new QVBoxLayout(advancedPage);
        m_debug = new QCheckBox(tr("Enable debug logging"));
        m_debug->setObjectName(QStringLiteral("debugEnabled"));
        m_debug->setToolTip(toolTipText(
            tr("Write diagnostic messages to KWin’s logs for troubleshooting. Leave off during normal use.")));
        advancedLayout->addWidget(m_debug);
        advancedLayout->addWidget(plainLabel(tr("Minimize2Tray, System Settings, configuration hosts, modal dialogs "
                                                "and system surfaces are protected from hiding.")));
        advancedLayout->addStretch();
        m_tabs->addTab(advancedPage, QIcon::fromTheme(QStringLiteral("configure")), tr("Advanced"));
        auto backupActions = new QHBoxLayout;
        auto import = makeButton(tr("Import configuration…"), QStringLiteral("document-import"));
        import->setObjectName(QStringLiteral("importConfiguration"));
        import->setToolTip(
            toolTipText(tr("Choose rules, options or both from a Minimize2Tray backup. Review before Apply or OK.")));
        auto exportConfig = makeButton(tr("Export configuration…"), QStringLiteral("document-export"));
        exportConfig->setObjectName(QStringLiteral("exportConfiguration"));
        exportConfig->setToolTip(toolTipText(tr("Back up all rules, options and sorting, including pending edits.")));
        backupActions->addWidget(import);
        backupActions->addWidget(exportConfig);
        backupActions->addStretch();
        outer->addLayout(backupActions);
        outer->addWidget(plainLabel(tr("Changes take effect after Apply or OK.")));
        connect(import, &QPushButton::clicked, this, &Minimize2TrayConfig::importConfiguration);
        connect(exportConfig, &QPushButton::clicked, this, [this]() { exportConfiguration(widget(), m_error); });
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
        editAction->setToolTip(toolTipText(tr("Open the selected rule’s matching properties and actions.")));
        auto deleteAction = new QAction(tr("Delete selected rule"), m_list);
        deleteAction->setToolTip(
            toolTipText(tr("Delete the selected rule. Undo delete is available until Apply or OK.")));
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
        connect(m_sort, &QComboBox::currentIndexChanged, this, [this]() {
            const auto selected = selectedId();
            const bool imported = !m_pendingSort.isEmpty();
            m_pendingSort.clear();
            const auto order = m_sort->currentData().toString();
            m_proxy->setOrder(order);
            // Persist this view preference immediately, independently of Apply/Cancel
            // or whether session shutdown runs the module destructor.
            auto state = uiState(QStringLiteral("page"));
            state.writeEntry("sort", order);
            state.sync();
            const int at = m_model->find(selected);
            if (at >= 0)
                m_list->setCurrentIndex(m_proxy->mapFromSource(m_model->index(at)));
            if (imported)
                changed();
        });
        widget()->installEventFilter(this);
        const auto state = uiState(QStringLiteral("page"));
        m_sort->setCurrentIndex(std::max(0, m_sort->findData(state.readEntry("sort", QStringLiteral("title")))));
        load();
        m_search->setText(state.readEntry("search", QString()));
        m_tabs->setCurrentIndex(std::clamp(state.readEntry("tab", 0), 0, 2));
        QTimer::singleShot(0, this,
                           [this, state]() { m_list->verticalScrollBar()->setValue(state.readEntry("scroll", 0)); });
        const int at = m_model->find(state.readEntry("selection", QString()));
        if (at >= 0)
            m_list->setCurrentIndex(m_proxy->mapFromSource(m_model->index(at)));
        // Configuration launched outside KWin is an ordinary managed window.
        // Repeated tray requests reuse it, even while there are no tray items.
        auto bus = QDBusConnection::sessionBus();
        if (QGuiApplication::platformName() != QLatin1String("org.kde.kwin.qpa") &&
            bus.registerObject(QStringLiteral("/Minimize2Tray/Configuration"), this,
                               QDBusConnection::ExportScriptableSlots)) {
            m_activationRegistered = bus.registerService(QString::fromLatin1(ConfigService));
            if (!m_activationRegistered)
                bus.unregisterObject(QStringLiteral("/Minimize2Tray/Configuration"));
        }
        if (args.contains(QStringLiteral("automatic-rule"))) {
            m_automaticHost = true;
            QTimer::singleShot(0, this, &Minimize2TrayConfig::addAutomaticRule);
        }
    }
    ~Minimize2TrayConfig() override {
        if (m_activationRegistered) {
            auto bus = QDBusConnection::sessionBus();
            bus.unregisterService(QString::fromLatin1(ConfigService));
            bus.unregisterObject(QStringLiteral("/Minimize2Tray/Configuration"));
        }
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
        m_pendingSort.clear();
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
        auto viewState = group.group(QStringLiteral("Ui")).group(QStringLiteral("page"));
        if (!m_pendingSort.isEmpty() && viewState.isEntryImmutable("sort")) {
            saveError(tr("The sorting preference is locked by the system administrator."));
            return;
        }
        const QString previousRules = current.value(QStringLiteral("rules")).toString();
        const auto previousSchema =
            QJsonDocument::fromJson(previousRules.toUtf8()).object().value(QStringLiteral("schemaVersion"));
        const QString backupKey = previousSchema == QJsonValue(1)   ? QStringLiteral("rulesSchema1Backup")
                                  : previousSchema == QJsonValue(2) ? QStringLiteral("rulesSchema2Backup")
                                  : previousSchema == QJsonValue(3) ? QStringLiteral("rulesSchema3Backup")
                                  : previousSchema == QJsonValue(4) ? QStringLiteral("rulesSchema4Backup")
                                  : previousSchema == QJsonValue(5) ? QStringLiteral("rulesSchema5Backup")
                                  : previousSchema == QJsonValue(6) ? QStringLiteral("rulesSchema6Backup")
                                                                    : QString();
        if (!backupKey.isEmpty() && !group.hasKey(backupKey)) {
            if (group.isEntryImmutable(backupKey)) {
                saveError(tr("The rule migration backup is locked by the system administrator."));
                return;
            }
            group.writeEntry(backupKey, previousRules);
        }
        group.writeEntry("rules", encodeRules(m_model->rules));
        group.writeEntry("countUseDot", m_dot->isChecked());
        group.writeEntry("countDisplayLimit", m_limit->value());
        group.writeEntry("hideOnMinimize", m_hide->isChecked());
        group.writeEntry("debugEnabled", m_debug->isChecked());
        if (!m_pendingSort.isEmpty())
            viewState.writeEntry("sort", m_pendingSort);
        if (!m_config.sync()) {
            m_config.markAsClean();
            saveError(tr("Saving failed. Your pending changes are still available; check that kwinrc is writable."));
            return;
        }
        const auto importedSort = m_pendingSort;
        if (!importedSort.isEmpty())
            KSharedConfig::openConfig(QStringLiteral("kwinrc"))->reparseConfiguration();
        load();
        if (!importedSort.isEmpty()) {
            // Already persisted with the rules and options in the same sync.
            const QSignalBlocker blocker(m_sort);
            m_sort->setCurrentIndex(m_sort->findData(importedSort));
            const auto selected = selectedId();
            m_proxy->setOrder(importedSort);
            const int at = m_model->find(selected);
            if (at >= 0)
                m_list->setCurrentIndex(m_proxy->mapFromSource(m_model->index(at)));
        }
    }
    void defaults() override {
        m_pendingSort.clear();
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

  public slots:
    Q_SCRIPTABLE void activate() {
        if (m_automaticBusy) {
            if (auto dialog = QApplication::activeModalWidget()) {
                dialog->raise();
                dialog->activateWindow();
            }
            return;
        }
        auto host = widget()->window();
        if (host->isMinimized())
            host->setWindowState(host->windowState() & ~Qt::WindowMinimized);
        host->show();
        host->raise();
        host->activateWindow();
    }

    Q_SCRIPTABLE void addAutomaticRule() {
        if (m_automaticBusy)
            return;
        // Do not save unrelated pending edits or interfere with another modal task.
        if (needsSave() || widget()->findChild<RuleDialog *>() || QApplication::activeModalWidget()) {
            m_automaticHost = false;
            activate();
            setMessage(
                m_error,
                tr("Finish the open dialog and apply or discard pending changes before adding an automatic rule."));
            return;
        }
        load();
        if (!m_loadError.isEmpty()) {
            m_automaticHost = false;
            activate();
            return;
        }
        m_automaticBusy = true;
        m_restoreAutomaticHost = !m_automaticHost && widget()->window()->isVisible();
        m_automaticLocker = std::make_unique<QEventLoopLocker>();
        widget()->window()->hide();
        auto message =
            QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"),
                                           QStringLiteral("org.kde.KWin"), QStringLiteral("queryWindowInfo"));
        auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 120000), this);
        whenFinished(watcher, this, [this](QDBusPendingCallWatcher *watcher) {
            const QDBusPendingReply<QVariantMap> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError()) {
                if (reply.error().name() == QLatin1String("org.kde.KWin.Error.UserCancel"))
                    finishAutomaticRule();
                else
                    automaticRuleError(tr("Window selection failed: %1").arg(reply.error().message()));
                return;
            }
            const auto id = QUuid(reply.value().value(QStringLiteral("uuid")).toString());
            if (id.isNull()) {
                automaticRuleError(tr("KWin did not return a valid application window."));
                return;
            }
            automaticWindows(id, [this](const QVariantMap &window, const QVariantList &) {
                QVariantList matches;
                for (const auto &entry : m_model->rules) {
                    auto enabled = entry.toMap();
                    enabled[QStringLiteral("enabled")] = true;
                    if (matchFailure(enabled, window).isEmpty())
                        matches.append(entry);
                }
                if (matches.isEmpty())
                    createAutomaticRule(window, {});
                else
                    chooseAutomaticRule(window, matches);
            });
        });
    }

  private:
    void finishAutomaticRule(bool keepConfiguration = false) {
        m_automaticBusy = false;
        if (keepConfiguration || m_restoreAutomaticHost)
            activate();
        else if (m_automaticHost)
            widget()->window()->close();
        m_automaticHost = false;
        m_automaticLocker.reset();
    }
    void automaticRuleError(const QString &error) {
        finishAutomaticRule(true);
        setMessage(m_error, error);
    }
    // KWin's picker omits minimizability/modal flags. Use the running script's
    // fresh window snapshot for eligibility, and recheck it before committing.
    void automaticWindows(const QUuid &id, std::function<void(const QVariantMap &, const QVariantList &)> next) {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Minimize2Tray"),
                                                      QStringLiteral("com.github.luisbocanegra.Minimize2Tray"),
                                                      QStringLiteral("windows"));
        auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 3000), this);
        whenFinished(watcher, this, [this, id, next](QDBusPendingCallWatcher *watcher) {
            const QDBusPendingReply<QVariantList> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError()) {
                automaticRuleError(tr("Enable Minimize2Tray before adding an automatic rule."));
                return;
            }
            QVariantList windows;
            QVariantMap selected;
            for (const auto &entry : reply.value()) {
                const auto window = entry.metaType().id() == qMetaTypeId<QDBusArgument>()
                                        ? qdbus_cast<QVariantMap>(entry)
                                        : entry.toMap();
                windows.append(window);
                if (QUuid(window.value(QStringLiteral("uuid")).toString()) == id)
                    selected = window;
            }
            if (selected.isEmpty()) {
                automaticRuleError(tr("The selected window has closed. Select a window again."));
                return;
            }
            // Validate type, child scope and required safety flags without any
            // application match restriction; identifiers are validated at save.
            auto eligible = newRule();
            eligible[QStringLiteral("match")] =
                QVariantMap{{QStringLiteral("windowTypes"), QStringList{windowType(selected)}},
                            {QStringLiteral("includeChildren"), true}};
            const auto failure = matchFailure(eligible, selected);
            if (!failure.isEmpty()) {
                automaticRuleError(failure);
                return;
            }
            next(selected, windows);
        });
    }
    void chooseAutomaticRule(const QVariantMap &window, const QVariantList &matches) {
        auto dialog = new QDialog(widget());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("existingAutomaticRule"));
        dialog->setWindowTitle(tr("Existing Tray Rule — Minimize to Tray"));
        dialog->setWindowRole(QStringLiteral("minimize2tray-existing-rule"));
        dialog->setWindowModality(Qt::WindowModal);
        auto layout = new QVBoxLayout(dialog);
        layout->addWidget(plainLabel(tr("A rule already matches this application/window and may also cover other "
                                        "windows. Edit it, create a separate narrower rule, or cancel. Existing "
                                        "rules continue applying when a narrower rule is created.")));
        auto choice = new QComboBox;
        choice->setObjectName(QStringLiteral("existingRuleChoice"));
        choice->setToolTip(
            toolTipText(tr("Choose the existing rule to edit or use as the starting point for a "
                           "narrower rule. Disabled rules are also listed; choosing one does not enable it.")));
        for (const auto &entry : matches) {
            const auto rule = entry.toMap();
            choice->addItem(ruleDisplayName(rule), rule);
            choice->setItemData(choice->count() - 1, toolTipText(ruleSummary(rule)), Qt::ToolTipRole);
        }
        layout->addWidget(choice);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        buttons->button(QDialogButtonBox::Cancel)
            ->setToolTip(
                toolTipText(tr("Stop without creating or changing a rule. The selected window is left as it is.")));
        auto edit = buttons->addButton(tr("Edit existing rule"), QDialogButtonBox::ActionRole);
        auto narrow = buttons->addButton(tr("Create a narrower rule"), QDialogButtonBox::ActionRole);
        edit->setObjectName(QStringLiteral("editExistingRule"));
        narrow->setObjectName(QStringLiteral("createNarrowerRule"));
        edit->setToolTip(toolTipText(
            tr("Open the selected rule for editing. Existing values are preserved; colored suggestions are not used "
               "unless you select Use. Confirm the editor, then choose Apply or OK in configuration to save. "
               "Changes affect every window matched by this rule; no new rule is created.")));
        narrow->setToolTip(toolTipText(
            tr("Save a new, more specific rule for the selected window and hide it now. The existing wider rule "
               "keeps working unchanged. A role or exact title limits the new rule; you can review it afterward.")));
        connect(edit, &QPushButton::clicked, dialog, [dialog]() { dialog->done(1); });
        connect(narrow, &QPushButton::clicked, dialog, [dialog]() { dialog->done(2); });
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(buttons);
        connect(dialog, &QDialog::finished, this, [this, choice, window](int result) {
            const auto existing = choice->currentData().toMap();
            if (result == 1) {
                finishAutomaticRule(true);
                openEditor(existing, window);
            } else if (result == 2) {
                createAutomaticRule(window, existing);
            } else {
                finishAutomaticRule();
            }
        });
        dialog->open();
    }
    void createAutomaticRule(const QVariantMap &picked, const QVariantMap &wider) {
        const auto id = QUuid(picked.value(QStringLiteral("uuid")).toString());
        automaticWindows(id, [this, picked, wider](const QVariantMap &window, const QVariantList &windows) {
            for (const auto &key :
                 {QStringLiteral("desktopFile"), QStringLiteral("resourceClass"), QStringLiteral("resourceName")}) {
                if (picked.value(key) != window.value(key)) {
                    automaticRuleError(tr("The selected window's application identity changed. Select it again."));
                    return;
                }
            }
            QString error;
            if (needsSave() || widget()->findChild<RuleDialog *>()) {
                automaticRuleError(tr("Apply or discard pending changes before adding an automatic rule."));
                return;
            }
            auto rule = Minimize2Tray::automaticRule(window, wider, windows, error);
            if (rule.isEmpty()) {
                automaticRuleError(error);
                return;
            }
            for (const auto &entry : m_model->rules) {
                if (QJsonObject::fromVariantMap(entry.toMap().value(QStringLiteral("match")).toMap()) ==
                    QJsonObject::fromVariantMap(rule.value(QStringLiteral("match")).toMap())) {
                    finishAutomaticRule(true);
                    openEditor(entry.toMap(), window);
                    return;
                }
            }
            if (m_model->rules.size() >= MaxRules) {
                automaticRuleError(tr("The maximum of 1024 rules has been reached."));
                return;
            }
            rule[QStringLiteral("title")] = appName(window).left(256);
            m_model->put(rule);
            // The ordinary save path rechecks disk state, immutability, schema
            // backups, size limits and the shared lock. No pending edits existed.
            save();
            if (needsSave()) {
                finishAutomaticRule(true);
                return;
            }
            auto message = QDBusMessage::createMethodCall(
                QStringLiteral("org.kde.KWin"), QStringLiteral("/Minimize2Tray"),
                QStringLiteral("com.github.luisbocanegra.Minimize2Tray"), QStringLiteral("applyAutomaticRule"));
            message.setArguments({window.value(QStringLiteral("uuid")), rule.value(QStringLiteral("id"))});
            auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 3000), this);
            whenFinished(watcher, this, [this, rule](QDBusPendingCallWatcher *watcher) {
                const QDBusPendingReply<bool> reply = *watcher;
                watcher->deleteLater();
                reviewAutomaticRule(rule, !reply.isError() && reply.value());
            });
        });
    }
    void reviewAutomaticRule(const QVariantMap &rule, bool hidden) {
        auto dialog = new QMessageBox(
            QMessageBox::Question, tr("Automatic Tray Rule Created"),
            hidden
                ? tr("The rule was saved and the selected window was minimized to the tray. Review or change the rule?")
                : tr("The rule was saved, but the selected window could not be hidden. It may have closed. Review or "
                     "change the rule?"),
            QMessageBox::NoButton, widget());
        dialog->setTextFormat(Qt::PlainText);
        dialog->setObjectName(QStringLiteral("reviewAutomaticRule"));
        dialog->setWindowRole(QStringLiteral("minimize2tray-review-rule"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        if (rule.value(QStringLiteral("match")).toMap().contains(QStringLiteral("caption")))
            dialog->setInformativeText(tr("This rule uses an exact window title. A changed title will stop matching."));
        auto review = dialog->addButton(tr("Review rule…"), QMessageBox::AcceptRole);
        auto keep = dialog->addButton(tr("Keep rule"), QMessageBox::RejectRole);
        dialog->setDefaultButton(keep);
        dialog->setEscapeButton(keep);
        review->setToolTip(toolTipText(
            tr("Open the rule that was just saved. It remains saved and active if you cancel the editor. "
               "Confirm the editor and choose Apply or OK in configuration to save edits. Reviewing it without "
               "edits keeps its Automatically created label.")));
        keep->setToolTip(
            toolTipText(tr("Keep the saved rule exactly as generated and finish without opening an editor. "
                           "Its Automatically created label stays. Escape also keeps the saved rule.")));
        connect(dialog, &QDialog::finished, this, [this, dialog, review, rule]() {
            const bool edit = dialog->clickedButton() == review;
            finishAutomaticRule(edit);
            if (edit)
                openEditor(rule);
        });
        dialog->open();
    }

  private slots:
    void importConfiguration() {
        if (!m_loadError.isEmpty()) {
            setMessage(m_error,
                       tr("Resolve the stored rule error before importing. View as text can save a recovery copy."));
            return;
        }
        const auto path = QFileDialog::getOpenFileName(widget(), tr("Import Minimize2Tray configuration"), QString(),
                                                       tr("Minimize2Tray configuration (*.json)"));
        if (path.isEmpty())
            return;
        ConfigurationBackup backup;
        QString error;
        if (!readConfigurationBackup(path, backup, error)) {
            setMessage(m_error, error);
            return;
        }
        auto dialog = new QDialog(widget());
        dialog->setObjectName(QStringLiteral("confirmConfigurationImport"));
        dialog->setWindowTitle(tr("Import configuration — Minimize to Tray"));
        dialog->setMinimumWidth(480);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        auto layout = new QVBoxLayout(dialog);
        layout->addWidget(
            plainLabel(tr("Backup rules: %1 · Current rules: %2").arg(backup.rules.size()).arg(m_model->rowCount())));
        auto rules = new QCheckBox(tr("Import window rules"));
        rules->setObjectName(QStringLiteral("importRules"));
        rules->setToolTip(toolTipText(tr("Import rules using Add or Replace below. Leave unchecked to keep the current "
                                         "rule list and its pending edits.")));
        rules->setChecked(true);
        layout->addWidget(rules);
        auto mode = new QComboBox;
        mode->setObjectName(QStringLiteral("importRuleMode"));
        mode->setToolTip(toolTipText(tr("Add keeps current rules and skips identical copies. Replace discards the "
                                        "current rule list and its pending edits in favour of the backup’s rules.")));
        mode->addItem(tr("Add to current rules"));
        mode->addItem(tr("Replace current rules"));
        comboToolTips(
            mode,
            {tr("Keep current rules and add new or changed rules; skip identical copies."),
             tr("Replace the editor’s current rule list with the backup’s rules. Apply or OK saves the replacement.")});
        auto form = new QFormLayout;
        form->addRow(tr("Rules:"), mode);
        layout->addLayout(form);
        auto duplicates = plainLabel(tr("Adding keeps current rules and skips identical copies. Changed versions "
                                        "are added separately."));
        layout->addWidget(duplicates);
        auto options = new QCheckBox(tr("Import application options (including sorting)"));
        options->setObjectName(QStringLiteral("importOptions"));
        options->setToolTip(toolTipText(tr("Replace the four application options and sorting with values from the "
                                           "backup. Leave unchecked to keep current values and pending edits.")));
        layout->addWidget(options);
        layout->addWidget(plainLabel(tr("Selected options replace current options. Unselected items keep their current "
                                        "values, including pending edits. Global shortcuts are not included.")));
        layout->addWidget(plainLabel(tr("Review after importing. Apply or OK saves; Cancel in the configuration "
                                        "window discards the import.")));
        auto errorLabel = plainLabel(QString());
        errorLabel->setObjectName(QStringLiteral("importError"));
        layout->addWidget(errorLabel);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Cancel)
            ->setToolTip(toolTipText(tr("Close without importing anything or changing pending edits.")));
        auto import = buttons->addButton(tr("Import"), QDialogButtonBox::ActionRole);
        import->setObjectName(QStringLiteral("applyConfigurationImport"));
        import->setToolTip(toolTipText(
            tr("Load the selected backup items into the editor for review. Apply or OK on the main page saves them.")));
        import->setAutoDefault(false);
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        auto update = [rules, options, mode, duplicates, import]() {
            mode->setEnabled(rules->isChecked());
            duplicates->setVisible(rules->isChecked() && mode->currentIndex() == 0);
            import->setEnabled(rules->isChecked() || options->isChecked());
        };
        connect(rules, &QCheckBox::toggled, dialog, update);
        connect(options, &QCheckBox::toggled, dialog, update);
        connect(mode, &QComboBox::currentIndexChanged, dialog, update);
        update();
        connect(import, &QPushButton::clicked, dialog, [this, dialog, rules, options, mode, errorLabel, backup]() {
            auto importedRules = backup.rules;
            if (rules->isChecked() && mode->currentIndex() == 0) {
                QString error;
                if (!mergeConfigurationRules(m_model->rules, backup.rules, importedRules, error)) {
                    setMessage(errorLabel, error);
                    return;
                }
            }
            m_loading = true;
            if (rules->isChecked()) {
                m_model->reset(importedRules);
                m_deleted.clear();
                m_undo->hide();
            }
            if (options->isChecked()) {
                m_dot->setChecked(backup.settings.value(QStringLiteral("countUseDot")).toBool());
                m_limit->setValue(backup.settings.value(QStringLiteral("countDisplayLimit")).toInt());
                m_hide->setChecked(backup.settings.value(QStringLiteral("hideOnMinimize")).toBool());
                m_debug->setChecked(backup.settings.value(QStringLiteral("debugEnabled")).toBool());
                // Keep imported sorting pending until Apply, just like other options.
                m_pendingSort = backup.sortOrder;
            }
            m_loading = false;
            changed();
            setMessage(m_error, tr("Selected items imported for review. Apply or OK saves the changes."));
            dialog->accept();
        });
        dialog->open();
    }
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
        setNeedsSave(values != m_loaded ||
                     (!m_pendingSort.isEmpty() && m_pendingSort != m_sort->currentData().toString()));
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
        text->setToolTip(toolTipText(tr("Read-only JSON for every rule, including disabled rules and pending edits. If stored rules are invalid, their original text is shown for recovery.")));
        text->setReadOnly(true);
        text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setPlainText(m_loadError.isEmpty() ? encodeRules(m_model->rules, true)
                                                 : m_loaded.value(QStringLiteral("rules")).toString());
        layout->addWidget(text, 1);
        auto error = plainLabel(QString());
        layout->addWidget(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        buttons->button(QDialogButtonBox::Close)->setToolTip(toolTipText(tr("Return to the configuration editor.")));
        auto copy = buttons->addButton(tr("Copy all"), QDialogButtonBox::ActionRole);
        copy->setToolTip(toolTipText(tr("Copy all the rule JSON shown here to the clipboard.")));
        auto save = buttons->addButton(tr("Save a copy…"), QDialogButtonBox::ActionRole);
        save->setToolTip(toolTipText(tr("Save only the rule text shown here. For an importable backup of rules and "
                                        "options, choose Export configuration.")));
        auto exportConfig = buttons->addButton(tr("Export configuration…"), QDialogButtonBox::ActionRole);
        exportConfig->setObjectName(QStringLiteral("exportTextConfiguration"));
        exportConfig->setToolTip(toolTipText(tr("Back up these rules together with all options and sorting.")));
        connect(exportConfig, &QPushButton::clicked, dialog,
                [this, dialog, error]() { exportConfiguration(dialog, error); });
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
    void exportConfiguration(QWidget *parent, QLabel *message) {
        if (!m_loadError.isEmpty()) {
            setMessage(message, tr("The stored rules are invalid. Use Save a copy in View as text for recovery."));
            return;
        }
        QString error;
        const auto json =
            encodeConfigurationBackup({m_model->rules, readSettingsFromWidgets(),
                                       m_pendingSort.isEmpty() ? m_sort->currentData().toString() : m_pendingSort},
                                      error);
        if (json.isEmpty()) {
            setMessage(message, error);
            return;
        }
        const auto path = QFileDialog::getSaveFileName(parent, tr("Export Minimize2Tray configuration"),
                                                       QStringLiteral("minimize2tray-config.json"),
                                                       tr("Minimize2Tray configuration (*.json)"));
        if (path.isEmpty())
            return;
        if (!writeConfigurationBackup(path, json, error))
            setMessage(message, error);
        else
            setMessage(message,
                       tr("Configuration exported, including pending edits. Global shortcuts are not included."));
    }
    void saveError(const QString &message) { setMessage(m_error, message); }
    void guardDialog(QDialog *host) {
        if (!host || host->property("minimize2traySaveGuard").toBool())
            return;
        host->setProperty("minimize2traySaveGuard", true);
        for (auto box : host->findChildren<QDialogButtonBox *>(QString(), Qt::FindDirectChildrenOnly)) {
            const QList<QPair<QDialogButtonBox::StandardButton, QString>> help{
                {QDialogButtonBox::Apply, tr("Save pending rules and options and apply them to Minimize2Tray.")},
                {QDialogButtonBox::Ok, tr("Save pending changes and close the configuration window.")},
                {QDialogButtonBox::Cancel, tr("Close without applying pending rule or option edits.")},
                {QDialogButtonBox::RestoreDefaults,
                 tr("Set pending rules and options to defaults. Apply or OK saves the changes.")},
                {QDialogButtonBox::Reset, tr("Reload saved rules and options, discarding pending edits.")}};
            for (const auto &entry : help)
                if (auto button = box->button(entry.first))
                    button->setToolTip(toolTipText(entry.second));
        }
        const bool deleteOnClose = host->testAttribute(Qt::WA_DeleteOnClose);
        // kcmshell6 normally quits when its last window closes. Keep its event
        // loop alive until the deferred save check can reopen a failed save.
        const bool ownHost = QCoreApplication::applicationName() == QLatin1String("kcmshell6");
        const auto quitLocker = ownHost && deleteOnClose && QApplication::quitOnLastWindowClosed()
                                    ? std::make_shared<QEventLoopLocker>() : nullptr;
        host->setAttribute(Qt::WA_DeleteOnClose, false);
        // KDE hosts can accept before and after save(); decide after both signals settle.
        connect(host, &QDialog::accepted, host, [host, deleteOnClose, quitLocker]() {
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

    void openEditor(const QVariantMap &rule, const QVariantMap &suggestions = {}) {
        auto dialog = new RuleDialog(rule, widget());
        if (!suggestions.isEmpty())
            dialog->suggestProperties(suggestions);
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
    RuleProxy *m_proxy;
    QComboBox *m_sort;
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
    QString m_pendingSort;
    QList<QPair<int, QVariantMap>> m_deleted;
    bool m_loading = false;
    bool m_uiHidden = false;
    QList<QMetaObject::Connection> m_stateConnections;
    bool m_activationRegistered = false;
    bool m_automaticBusy = false;
    bool m_automaticHost = false;
    bool m_restoreAutomaticHost = false;
    std::unique_ptr<QEventLoopLocker> m_automaticLocker;
    QString m_savedSearch;
    QString m_savedSelection;
    int m_savedTab = 0;
    int m_savedScroll = 0;
};

K_PLUGIN_CLASS_WITH_JSON(Minimize2TrayConfig, "configmodule.json")
#include "configmodule.moc"
