/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerLayoutView.h"

#include "common/LayoutLinks.h"
#include "dialogs/ComputerSettingsDialog.h"
#include "gui/config/ComputerList.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QGraphicsRectItem>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

using namespace deskflow::layout;

namespace {
// Distance in view pixels within which a dragged computer snaps to a neighbour.
constexpr int kSnapPixels = 12;
constexpr int kMarginPixels = 16;
// Width in view pixels of the right and bottom borders that resize a computer.
constexpr int kHandlePixels = 14;
constexpr QSize kMinimumSize{320, 180};
} // namespace

class ComputerItem : public QGraphicsRectItem
{
public:
  ComputerItem(int index, const Computer &computer)
      : QGraphicsRectItem(QRectF(QPointF(), computer.geometry().size())),
        m_index(index),
        m_name(computer.name()),
        m_pixmap(computer.pixmap()),
        m_isServer(computer.isServer())
  {
    setPos(computer.geometry().topLeft());
    setFlags(ItemIsMovable | ItemIsSelectable);
    setCursor(Qt::OpenHandCursor);
    setToolTip(
        QObject::tr(
            "<center>Computer: <b>%1</b></center>"
            "<br>Drag next to another computer to link their edges"
            "<br>Drag the right or bottom edge to resize"
            "<br>Double click to edit settings"
            "<br>Drag computer to the trashcan to remove it"
        )
            .arg(m_name)
    );
  }

  int index() const
  {
    return m_index;
  }

  bool isServer() const
  {
    return m_isServer;
  }

  void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override
  {
    // Draw in device pixels so the icon and name stay readable however far the layout is zoomed out.
    const QRectF device = painter->worldTransform().mapRect(rect()).adjusted(1, 1, -1, -1);
    const auto &palette = widget != nullptr ? widget->palette() : option->palette;

    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing);
    auto border = palette.color(QPalette::Text);
    border.setAlphaF(0.35f);
    painter->setPen(m_isServer ? QPen(palette.color(QPalette::Highlight), 2) : QPen(border, 1));
    painter->setBrush(palette.color(isSelected() ? QPalette::Midlight : QPalette::Button));
    painter->drawRoundedRect(device, 4, 4);

    const int textHeight = painter->fontMetrics().height();
    const int iconSize = std::min(64, static_cast<int>(device.height()) - textHeight - 8);
    if (iconSize >= 16) {
      const QRect iconRect(
          static_cast<int>(device.center().x()) - iconSize / 2,
          static_cast<int>(device.center().y()) - (iconSize + textHeight) / 2, iconSize, iconSize
      );
      painter->drawPixmap(iconRect, m_pixmap);
    }
    const QRectF textRect(
        device.left() + 4, device.center().y() + iconSize / 2.0 - textHeight / 2.0, device.width() - 8, textHeight
    );
    painter->setPen(palette.color(QPalette::Text));
    painter->drawText(
        textRect, Qt::AlignCenter,
        painter->fontMetrics().elidedText(m_name, Qt::ElideMiddle, static_cast<int>(textRect.width()))
    );

    painter->setPen(border);
    for (int offset = 4; offset < kHandlePixels; offset += 4) {
      painter->drawLine(device.bottomRight() - QPointF(offset, 2), device.bottomRight() - QPointF(2, offset));
    }
    painter->restore();
  }

private:
  int m_index;
  QString m_name;
  QPixmap m_pixmap;
  bool m_isServer;
};

ComputerLayoutView::ComputerLayoutView(QWidget *parent) : QGraphicsView(parent)
{
  setScene(new QGraphicsScene(this));
  setAcceptDrops(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setRenderHint(QPainter::Antialiasing);
  setAlignment(Qt::AlignCenter);
}

const QString &ComputerLayoutView::mimeType()
{
  static const QString type = QStringLiteral("application/x-deskflow-computer");
  return type;
}

void ComputerLayoutView::setComputers(ComputerList *computers)
{
  m_computers = computers;
  rebuild();
}

void ComputerLayoutView::rebuild()
{
  m_dragged = nullptr;
  m_resized = nullptr;
  scene()->clear();
  if (m_computers == nullptr)
    return;

  for (int i = 0; i < m_computers->size(); i++) {
    const auto &computer = m_computers->at(i);
    if (!computer.isNull())
      scene()->addItem(new ComputerItem(i, computer));
  }
  fit();
}

void ComputerLayoutView::fit()
{
  const auto bounds = scene()->itemsBoundingRect();
  if (bounds.isEmpty())
    return;

  const auto scale = std::min(
      (viewport()->width() - 2.0 * kMarginPixels) / bounds.width(),
      (viewport()->height() - 2.0 * kMarginPixels) / bounds.height()
  );
  if (scale <= 0)
    return;
  const auto margin = kMarginPixels / scale;
  const auto padded = bounds.adjusted(-margin, -margin, margin, margin);
  scene()->setSceneRect(padded);
  fitInView(padded, Qt::KeepAspectRatio);
}

void ComputerLayoutView::removeComputer(int index)
{
  if (m_computers->at(index).isServer())
    return;
  m_computers->removeAt(index);
  rebuild();
  Q_EMIT computersChanged();
}

ComputerItem *ComputerLayoutView::itemAt(const QPoint &pos) const
{
  return dynamic_cast<ComputerItem *>(QGraphicsView::itemAt(pos));
}

Qt::Edges ComputerLayoutView::resizeEdges(const ComputerItem *item, const QPoint &pos) const
{
  const auto corner = mapFromScene(item->mapToScene(item->rect().bottomRight()));
  Qt::Edges edges;
  if (pos.x() <= corner.x() && corner.x() - pos.x() < kHandlePixels)
    edges |= Qt::RightEdge;
  if (pos.y() <= corner.y() && corner.y() - pos.y() < kHandlePixels)
    edges |= Qt::BottomEdge;
  return edges;
}

Qt::CursorShape ComputerLayoutView::resizeCursor(Qt::Edges edges)
{
  if (edges == (Qt::RightEdge | Qt::BottomEdge))
    return Qt::SizeFDiagCursor;
  if (edges == Qt::RightEdge)
    return Qt::SizeHorCursor;
  if (edges == Qt::BottomEdge)
    return Qt::SizeVerCursor;
  return Qt::OpenHandCursor;
}

void ComputerLayoutView::finishResize(ComputerItem *item)
{
  auto &computer = (*m_computers)[item->index()];
  const auto others = m_computers->rects(computer.name()).values();
  const QRect resized(item->pos().toPoint(), item->rect().size().toSize());
  const auto snapped = snapResize(resized, others, qRound(kSnapPixels / transform().m11()));
  if (snapped != computer.geometry() && !overlapsAny(snapped, others)) {
    computer.setGeometry(snapped);
    Q_EMIT computersChanged();
  }
  rebuild();
}

void ComputerLayoutView::dragOut(ComputerItem *item)
{
  // Leaving the view hands the computer over to a real drag so it can follow the cursor onto the trash.
  const int index = item->index();
  m_dragged = nullptr;
  if (auto *grabber = scene()->mouseGrabberItem(); grabber != nullptr)
    grabber->ungrabMouse();

  auto *mimeData = new QMimeData();
  mimeData->setData(mimeType(), {});
  auto *drag = new QDrag(this);
  drag->setMimeData(mimeData);
  drag->setPixmap(m_computers->at(index).pixmap());
  if (drag->exec(Qt::MoveAction) == Qt::MoveAction)
    removeComputer(index);
  else
    rebuild();
}

void ComputerLayoutView::resizeEvent(QResizeEvent *event)
{
  QGraphicsView::resizeEvent(event);
  fit();
}

void ComputerLayoutView::mousePressEvent(QMouseEvent *event)
{
  auto *item = event->button() == Qt::LeftButton ? itemAt(event->pos()) : nullptr;
  if (item != nullptr && resizeEdges(item, event->pos())) {
    m_resized = item;
    m_resizeEdges = resizeEdges(item, event->pos());
    return;
  }
  m_dragged = item;
  QGraphicsView::mousePressEvent(event);
}

void ComputerLayoutView::mouseMoveEvent(QMouseEvent *event)
{
  if (m_resized != nullptr) {
    const auto cursor = mapToScene(event->pos()) - m_resized->pos();
    auto size = m_resized->rect().size();
    if (m_resizeEdges & Qt::RightEdge)
      size.setWidth(std::max<qreal>(cursor.x(), kMinimumSize.width()));
    if (m_resizeEdges & Qt::BottomEdge)
      size.setHeight(std::max<qreal>(cursor.y(), kMinimumSize.height()));
    m_resized->setRect(QRectF(QPointF(), size));
    return;
  }

  if (m_dragged != nullptr && !m_dragged->isServer() && !viewport()->rect().contains(event->pos())) {
    dragOut(m_dragged);
    return;
  }

  if (auto *item = itemAt(event->pos()); item != nullptr && event->buttons() == Qt::NoButton)
    item->setCursor(resizeCursor(resizeEdges(item, event->pos())));
  QGraphicsView::mouseMoveEvent(event);
}

void ComputerLayoutView::mouseReleaseEvent(QMouseEvent *event)
{
  if (auto *resized = std::exchange(m_resized, nullptr); resized != nullptr) {
    finishResize(resized);
    return;
  }

  QGraphicsView::mouseReleaseEvent(event);
  auto *item = std::exchange(m_dragged, nullptr);
  if (item == nullptr || event->button() != Qt::LeftButton)
    return;

  auto &computer = (*m_computers)[item->index()];

  const auto others = m_computers->rects(computer.name()).values();
  const QRect moved(item->pos().toPoint(), computer.geometry().size());
  const auto snapped = snap(moved, others, qRound(kSnapPixels / transform().m11()));
  if (snapped != computer.geometry() && !overlapsAny(snapped, others)) {
    computer.setGeometry(snapped);
    Q_EMIT computersChanged();
  }
  // Redraw every item from the stored geometry, which also undoes moves of other selected items.
  rebuild();
}

void ComputerLayoutView::mouseDoubleClickEvent(QMouseEvent *event)
{
  auto *item = itemAt(event->pos());
  if (item == nullptr || event->button() != Qt::LeftButton) {
    QGraphicsView::mouseDoubleClickEvent(event);
    return;
  }

  ComputerSettingsDialog dialog(this, &(*m_computers)[item->index()], m_computers);
  dialog.exec();
  rebuild();
  Q_EMIT computersChanged();
}

void ComputerLayoutView::keyPressEvent(QKeyEvent *event)
{
  const auto selected = scene()->selectedItems();
  if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected.size() == 1) {
    removeComputer(static_cast<ComputerItem *>(selected.first())->index());
    return;
  }
  QGraphicsView::keyPressEvent(event);
}

void ComputerLayoutView::dragEnterEvent(QDragEnterEvent *event)
{
  dragMoveEvent(event);
}

void ComputerLayoutView::dragMoveEvent(QDragMoveEvent *event)
{
  if (m_computers != nullptr && event->source() != this && event->mimeData()->hasFormat(mimeType()))
    event->acceptProposedAction();
  else
    event->ignore();
}

void ComputerLayoutView::dropEvent(QDropEvent *event)
{
  if (m_computers == nullptr || !event->mimeData()->hasFormat(mimeType())) {
    event->ignore();
    return;
  }

  auto encoded = event->mimeData()->data(mimeType());
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  int unusedColumn = -1;
  int unusedRow = -1;
  Computer computer;
  stream >> unusedColumn >> unusedRow >> computer;

  const auto size = ComputerList::kDefaultSize;
  const auto center = mapToScene(event->position().toPoint()).toPoint();
  const auto others = m_computers->rects().values();
  const auto placed = snap(
      QRect(center - QPoint(size.width() / 2, size.height() / 2), size), others, qRound(kSnapPixels / transform().m11())
  );
  if (overlapsAny(placed, others)) {
    m_computers->addComputerByPriority(computer);
  } else {
    computer.setGeometry(placed);
    m_computers->append(computer);
  }

  event->acceptProposedAction();
  rebuild();
  Q_EMIT computersChanged();
}
