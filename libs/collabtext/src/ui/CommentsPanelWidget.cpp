#include "ui/CommentsPanelWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

namespace CollabText::Ui {

CommentsPanelWidget::CommentsPanelWidget(QWidget *parent)
    : QWidget(parent)
    , m_toggleResolved(new QPushButton(this))
    , m_list(new QListWidget(this))
    , m_input(new QLineEdit(this))
{
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);

    m_input->setPlaceholderText("Add comment on selection...");

    auto *header = new QLabel("Comments", this);
    QFont f = header->font();
    f.setBold(true);
    header->setFont(f);

    m_toggleResolved->setCheckable(true);
    m_toggleResolved->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(header);
    layout->addWidget(m_toggleResolved);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_input);

    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        const QString text = m_input->text().trimmed();
        if (!text.isEmpty()) {
            emit addCommentRequested(text);
            m_input->clear();
        }
    });

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        emit commentClicked(id);
    });

    connect(m_toggleResolved, &QPushButton::toggled, this, [this](bool checked) {
        m_showResolved = checked;
        rebuildList();
    });
}

void CommentsPanelWidget::setComments(const QList<CommentDisplayInfo> &comments)
{
    m_all = comments;
    rebuildList();
}

void CommentsPanelWidget::rebuildList()
{
    m_list->clear();

    int resolvedCount = 0;
    for (const CommentDisplayInfo &info : m_all)
        if (info.resolved) ++resolvedCount;

    if (resolvedCount == 0) {
        m_toggleResolved->setVisible(false);
        m_toggleResolved->setChecked(false);
        m_showResolved = false;
    } else {
        m_toggleResolved->setVisible(true);
        m_toggleResolved->setText(
            (m_showResolved ? "Hide resolved (" : "Show resolved (")
            + QString::number(resolvedCount) + ")");
    }

    for (const CommentDisplayInfo &info : m_all) {
        if (info.resolved && !m_showResolved) continue;

        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, info.id);

        const QString authorColorHex = info.authorColor.name();
        const QString textColor = info.resolved ? "#888" : authorColorHex;
        const QString fontStyle = info.resolved ? "font-style:italic;color:#888;" : "";
        const QString resolvedTag = info.resolved
            ? QStringLiteral(" <span style=\"color:#888\">(resolved)</span>")
            : QString();

        QString html = QString("<b style=\"color:%1\">%2</b>%3 <span style=\"%4\">%5</span>")
            .arg(textColor,
                 info.authorName.toHtmlEscaped(),
                 resolvedTag,
                 fontStyle,
                 info.body.toHtmlEscaped());

        if (!info.contextSnippet.isEmpty()) {
            html += QString("<br><span style=\"color:#888\">&gt; %1</span>")
                .arg(info.contextSnippet.toHtmlEscaped());
        }

        auto *row = new QWidget(m_list);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 2, 2, 2);
        rowLayout->setSpacing(4);

        auto *label = new QLabel(row);
        label->setTextFormat(Qt::RichText);
        label->setText(html);
        label->setWordWrap(true);

        auto *resolveBtn = new QToolButton(row);
        resolveBtn->setText(info.resolved ? "Unresolve" : "Resolve");
        resolveBtn->setAutoRaise(true);

        auto *deleteBtn = new QToolButton(row);
        deleteBtn->setText("✕");
        deleteBtn->setAutoRaise(true);
        deleteBtn->setToolTip("Delete comment");

        rowLayout->addWidget(label, 1);
        rowLayout->addWidget(resolveBtn);
        rowLayout->addWidget(deleteBtn);

        const QString id = info.id;
        const bool wasResolved = info.resolved;
        connect(resolveBtn, &QToolButton::clicked, this, [this, id, wasResolved]() {
            if (wasResolved) emit unresolveRequested(id);
            else             emit resolveRequested(id);
        });
        connect(deleteBtn, &QToolButton::clicked, this, [this, id]() {
            emit deleteRequested(id);
        });

        item->setSizeHint(row->sizeHint());
        m_list->setItemWidget(item, row);
    }
}

} // namespace CollabText::Ui
