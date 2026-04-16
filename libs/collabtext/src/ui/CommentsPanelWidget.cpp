#include "ui/CommentsPanelWidget.h"

#include <QLabel>
#include <QListWidgetItem>
#include <QVBoxLayout>

namespace CollabText::Ui {

CommentsPanelWidget::CommentsPanelWidget(QWidget *parent)
    : QWidget(parent)
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

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(header);
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
}

void CommentsPanelWidget::setComments(const QList<CommentDisplayInfo> &comments)
{
    m_list->clear();

    for (const CommentDisplayInfo &info : comments) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, info.id);

        QString html = QString("<b style=\"color:%1\">%2</b> %3")
            .arg(info.authorColor.name(),
                 info.authorName.toHtmlEscaped(),
                 info.body.toHtmlEscaped());

        if (!info.contextSnippet.isEmpty()) {
            html += QString("<br><span style=\"color:#888\">&gt; %1</span>")
                .arg(info.contextSnippet.toHtmlEscaped());
        }

        auto *label = new QLabel(this);
        label->setTextFormat(Qt::RichText);
        label->setText(html);
        label->setWordWrap(true);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);

        item->setSizeHint(label->sizeHint());
        m_list->setItemWidget(item, label);
    }
}

} // namespace CollabText::Ui
