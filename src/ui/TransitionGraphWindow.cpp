// Interactive force-directed transition graph used to plan a DJ set and
// estimate the longest timing-feasible route from any hovered song.
#include "TransitionGraphWindow.h"

#include "Theme.h"
#include "../audio/AudioEngine.h"
#include "../library/TrackLibrary.h"
#include "../transitions/TransitionGraph.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace gvt {
namespace {

QString routeDurationText(double seconds)
{
    const int total = std::max(0, static_cast<int>(std::lround(seconds)));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int remainder = total % 60;
    if (hours > 0)
        return QObject::tr("%1 h %2 min").arg(hours).arg(minutes);
    if (minutes > 0)
        return QObject::tr("%1 min %2 sec").arg(minutes).arg(remainder);
    return QObject::tr("%1 sec").arg(remainder);
}

QPointF rectBoundary(const QPointF& center, const QSizeF& size,
                     const QPointF& toward)
{
    const QPointF delta = toward - center;
    if (std::fabs(delta.x()) < 0.001 && std::fabs(delta.y()) < 0.001)
        return center;
    const double halfWidth = std::max(1.0, size.width() / 2.0);
    const double halfHeight = std::max(1.0, size.height() / 2.0);
    const double scale = 1.0 / std::max(std::fabs(delta.x()) / halfWidth,
                                        std::fabs(delta.y()) / halfHeight);
    return center + delta * scale;
}

} // namespace

class TransitionGraphWindow::Canvas final : public QWidget {
public:
    explicit Canvas(QWidget* parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("transitionGraphCanvas"));
        setMinimumSize(600, 420);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setProperty("zoomPercent", 100);
        simulation_.setInterval(16);
        connect(&simulation_, &QTimer::timeout, this, [this] {
            stepSimulation();
        });
        simulation_.start();
    }

    void setZoomPercent(int percent)
    {
        changeZoom(static_cast<double>(percent) / 100.0,
                   QPointF(width() / 2.0, height() / 2.0), false);
    }

    void setZoomChangedCallback(std::function<void(int)> callback)
    {
        zoomChanged_ = std::move(callback);
    }

    void setGraph(TransitionGraph graph)
    {
        std::map<QString, QPointF> oldPositions;
        for (std::size_t index = 0;
             index < graph_.nodes.size() && index < visual_.size(); ++index)
            oldPositions.emplace(graph_.nodes[index].key,
                                 visual_[index].position);

        graph_ = std::move(graph);
        visual_.clear();
        visual_.reserve(graph_.nodes.size());
        const double radius = std::max(130.0,
            42.0 * static_cast<double>(graph_.nodes.size()));
        const QFontMetrics metrics(font());
        for (std::size_t index = 0; index < graph_.nodes.size(); ++index) {
            VisualNode node;
            const auto old = oldPositions.find(graph_.nodes[index].key);
            if (old != oldPositions.end()) {
                node.position = old->second;
            } else {
                const double angle = graph_.nodes.size() > 1
                    ? 2.0 * 3.14159265358979323846 *
                          static_cast<double>(index) /
                          static_cast<double>(graph_.nodes.size())
                    : 0.0;
                node.position = QPointF(std::cos(angle) * radius,
                                        std::sin(angle) * radius);
            }
            node.size = QSizeF(
                std::clamp(metrics.horizontalAdvance(
                               graph_.nodes[index].title) + 32.0,
                           96.0, 210.0),
                46.0);
            visual_.push_back(node);
        }
        hoveredNode_ = -1;
        draggedNode_ = -1;
        routeCache_.clear();
        setProperty("nodeCount", static_cast<int>(graph_.nodes.size()));
        setProperty("edgeCount", static_cast<int>(graph_.edges.size()));
        setProperty("edgeLabelCount", static_cast<int>(graph_.edges.size()));
        setProperty("hoveredNodeIndex", -1);
        setProperty("hoverRouteNodeCount", 0);
        setProperty("hoverRouteDurationSeconds", 0.0);
        setProperty("panOffset", pan_);
        update();
    }

    const TransitionGraph& graph() const noexcept { return graph_; }

    void setCurrentDeckNodes(int deckA, int deckB)
    {
        const std::array<int, 2> next {deckA, deckB};
        if (currentDeckNodes_ == next) return;
        currentDeckNodes_ = next;
        setProperty("currentDeckANode", deckA);
        setProperty("currentDeckBNode", deckB);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        struct EdgeLabel {
            QPointF worldPosition;
            QString text;
            bool highlighted = false;
        };

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(17, 19, 24));

        if (graph_.nodes.empty()) {
            painter.setPen(themeDimText());
            painter.drawText(rect(), Qt::AlignCenter,
                             tr("No transition edges yet"));
            return;
        }

        painter.save();
        painter.setTransform(worldTransform());

        const TransitionGraphRoute route = hoveredRoute();
        const std::set<int> routeEdges(route.edges.begin(), route.edges.end());
        const std::set<int> routeNodes(route.nodes.begin(), route.nodes.end());
        std::vector<EdgeLabel> edgeLabels;
        edgeLabels.reserve(graph_.edges.size());
        for (int edgeIndex = 0;
             edgeIndex < static_cast<int>(graph_.edges.size()); ++edgeIndex) {
            const TransitionGraphEdge& edge =
                graph_.edges[static_cast<std::size_t>(edgeIndex)];
            if (edge.from < 0 || edge.to < 0 ||
                edge.from >= static_cast<int>(visual_.size()) ||
                edge.to >= static_cast<int>(visual_.size()))
                continue;
            const bool highlighted = routeEdges.contains(edgeIndex);
            const VisualNode& from = visual_[static_cast<std::size_t>(edge.from)];
            const VisualNode& to = visual_[static_cast<std::size_t>(edge.to)];
            const QPointF start = rectBoundary(from.position, from.size,
                                               to.position);
            const QPointF finish = rectBoundary(to.position, to.size,
                                                from.position);
            const QPointF delta = finish - start;
            const double length = std::hypot(delta.x(), delta.y());
            if (length < 1.0) continue;
            const QPointF normal(-delta.y() / length, delta.x() / length);
            int parallelCount = 0;
            int parallelIndex = 0;
            bool reverseExists = false;
            for (int other = 0;
                 other < static_cast<int>(graph_.edges.size()); ++other) {
                const TransitionGraphEdge& candidate =
                    graph_.edges[static_cast<std::size_t>(other)];
                if (candidate.from == edge.from && candidate.to == edge.to) {
                    if (other < edgeIndex) ++parallelIndex;
                    ++parallelCount;
                } else if (candidate.from == edge.to &&
                           candidate.to == edge.from) {
                    reverseExists = true;
                }
            }
            double offset = (static_cast<double>(parallelIndex) -
                             (parallelCount - 1) / 2.0) * 17.0;
            if (reverseExists)
                offset += edge.from < edge.to ? 10.0 : -10.0;
            const QPointF control = (start + finish) / 2.0 + normal * offset;

            QColor edgeColor = highlighted ? QColor(53, 200, 232)
                                           : QColor(92, 101, 117);
            if (hoveredNode_ >= 0 && !highlighted) edgeColor.setAlpha(85);
            QPen edgePen(edgeColor, highlighted ? 3.0 : 1.4,
                         Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(edgePen);
            painter.setBrush(Qt::NoBrush);
            QPainterPath path(start);
            path.quadTo(control, finish);
            painter.drawPath(path);

            edgeLabels.push_back({
                path.pointAtPercent(0.5),
                edge.name.isEmpty() ? tr("Untitled transition") : edge.name,
                highlighted,
            });

            QPointF tangent = finish - control;
            const double tangentLength =
                std::hypot(tangent.x(), tangent.y());
            if (tangentLength > 0.001) tangent /= tangentLength;
            const QPointF arrowNormal(-tangent.y(), tangent.x());
            const double arrowSize = highlighted ? 10.0 : 8.0;
            QPolygonF arrow;
            arrow << finish
                  << finish - tangent * arrowSize +
                         arrowNormal * arrowSize * 0.55
                  << finish - tangent * arrowSize -
                         arrowNormal * arrowSize * 0.55;
            painter.setPen(Qt::NoPen);
            painter.setBrush(edgeColor);
            painter.drawPolygon(arrow);
        }

        // Small, muted captions sit just above the edges. Keep their screen
        // size stable on zoom and draw song nodes last for visual priority.
        painter.restore();
        QFont edgeFont = font();
        edgeFont.setPixelSize(9);
        edgeFont.setBold(false);
        painter.setFont(edgeFont);
        const QFontMetrics edgeMetrics(edgeFont);
        const QTransform transform = worldTransform();
        for (const EdgeLabel& label : edgeLabels) {
            const QString text = edgeMetrics.elidedText(
                label.text, Qt::ElideRight, 160);
            const QSize textSize = edgeMetrics.size(Qt::TextSingleLine, text);
            const QPointF center = transform.map(label.worldPosition);
            const QRectF caption(center.x() - textSize.width() / 2.0,
                                 center.y() - textSize.height() - 3.0,
                                 textSize.width(), textSize.height());
            QColor textColor = label.highlighted
                ? QColor(146, 183, 194, 180) : QColor(139, 148, 164, 140);
            if (hoveredNode_ >= 0 && !label.highlighted)
                textColor.setAlpha(70);
            painter.setPen(textColor);
            painter.drawText(caption, Qt::AlignCenter, text);
        }

        painter.save();
        painter.setTransform(worldTransform());
        painter.setFont(font());

        const QFontMetrics metrics(font());
        for (int index = 0; index < static_cast<int>(visual_.size()); ++index) {
            const VisualNode& node = visual_[static_cast<std::size_t>(index)];
            const QRectF bounds(node.position.x() - node.size.width() / 2.0,
                                node.position.y() - node.size.height() / 2.0,
                                node.size.width(), node.size.height());
            const bool hovered = index == hoveredNode_;
            const bool routed = routeNodes.contains(index);
            const bool loadedA = index == currentDeckNodes_[0];
            const bool loadedB = index == currentDeckNodes_[1];
            painter.setPen(QPen(hovered ? QColor(241, 199, 91)
                                       : routed ? QColor(53, 200, 232)
                                                : QColor(91, 101, 118),
                                hovered ? 3.0 : routed ? 2.5 : 1.4));
            painter.setBrush(hovered ? QColor(46, 49, 46)
                                     : routed ? QColor(26, 58, 67)
                                              : QColor(37, 41, 49));
            painter.drawRoundedRect(bounds, 13.0, 13.0);

            // A loaded song stays obvious even when hover route styling is
            // also active. Deck-colored outer rings and letter badges remain
            // distinct from the route's cyan fill/border treatment.
            if (loadedA || loadedB) {
                const QRectF outer = bounds.adjusted(-4.0, -4.0, 4.0, 4.0);
                if (loadedA && loadedB) {
                    QLinearGradient split(outer.topLeft(), outer.topRight());
                    split.setColorAt(0.0, deckAccent(0));
                    split.setColorAt(0.48, deckAccent(0));
                    split.setColorAt(0.52, deckAccent(1));
                    split.setColorAt(1.0, deckAccent(1));
                    painter.setPen(QPen(QBrush(split), 3.5));
                } else {
                    painter.setPen(QPen(deckAccent(loadedA ? 0 : 1), 3.5));
                }
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(outer, 16.0, 16.0);
            }

            painter.setPen(hovered || routed ? QColor(238, 242, 247)
                                             : themeText());
            const int badgeCount = static_cast<int>(loadedA) +
                                   static_cast<int>(loadedB);
            const QString title = metrics.elidedText(
                graph_.nodes[static_cast<std::size_t>(index)].title,
                Qt::ElideRight,
                static_cast<int>(node.size.width() - 22.0 -
                                 badgeCount * 21.0));
            painter.drawText(bounds.adjusted(
                                 11.0, 0.0,
                                 -11.0 - badgeCount * 21.0, 0.0),
                             Qt::AlignCenter, title);

            int badgeRight = static_cast<int>(bounds.right()) - 7;
            const auto drawBadge = [&](int deck) {
                const QRect badge(badgeRight - 17,
                                  static_cast<int>(bounds.center().y()) - 9,
                                  18, 18);
                painter.setPen(Qt::NoPen);
                painter.setBrush(deckAccent(deck));
                painter.drawEllipse(badge);
                painter.setPen(QColor(13, 16, 20));
                QFont badgeFont = painter.font();
                badgeFont.setBold(true);
                painter.setFont(badgeFont);
                painter.drawText(badge, Qt::AlignCenter,
                                 deck == 0 ? QStringLiteral("A")
                                           : QStringLiteral("B"));
                painter.setFont(font());
                badgeRight -= 21;
            };
            if (loadedB) drawBadge(1);
            if (loadedA) drawBadge(0);
        }
        painter.restore();

        if (!visual_.empty()) {
            setProperty("node0Center",
                        worldTransform().map(visual_.front().position));
        }

        if (hoveredNode_ >= 0) drawRouteCard(painter, route);
        painter.setPen(QColor(130, 139, 153));
        painter.drawText(QRect(12, 9, width() - 24, 22),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         tr("Drag songs to organise · drag empty space to pan · scroll to zoom"));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) return;
        setFocus();
        lastPointer_ = event->position();
        draggedNode_ = nodeAt(event->position());
        if (draggedNode_ >= 0) {
            visual_[static_cast<std::size_t>(draggedNode_)].velocity = {};
        } else {
            panning_ = true;
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (draggedNode_ >= 0) {
            VisualNode& node =
                visual_[static_cast<std::size_t>(draggedNode_)];
            node.position = toWorld(event->position());
            node.velocity = {};
            update();
            event->accept();
            return;
        }
        if (panning_) {
            pan_ += event->position() - lastPointer_;
            lastPointer_ = event->position();
            setProperty("panOffset", pan_);
            update();
            event->accept();
            return;
        }

        const int hover = nodeAt(event->position());
        if (hover != hoveredNode_) {
            hoveredNode_ = hover;
            setProperty("hoveredNodeIndex", hover);
            if (hover >= 0) {
                const TransitionGraphRoute route = routeFor(hover);
                setProperty("hoverRouteNodeCount",
                            static_cast<int>(route.nodes.size()));
                setProperty("hoverRouteDurationSeconds",
                            route.durationSeconds);
            } else {
                setProperty("hoverRouteNodeCount", 0);
                setProperty("hoverRouteDurationSeconds", 0.0);
            }
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) return;
        draggedNode_ = -1;
        panning_ = false;
        event->accept();
    }

    void leaveEvent(QEvent*) override
    {
        if (draggedNode_ >= 0 || panning_) return;
        hoveredNode_ = -1;
        setProperty("hoveredNodeIndex", -1);
        setProperty("hoverRouteNodeCount", 0);
        setProperty("hoverRouteDurationSeconds", 0.0);
        update();
    }

    void wheelEvent(QWheelEvent* event) override
    {
        double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
        if (std::fabs(steps) < 0.0001)
            steps = static_cast<double>(event->pixelDelta().y()) / 120.0;
        if (std::fabs(steps) < 0.0001) return;
        // A conventional wheel notch changes scale by only four percent. This
        // also makes high-resolution trackpads smooth instead of jumpy.
        changeZoom(zoom_ * std::pow(1.04, steps), event->position(), true);
        event->accept();
    }

private:
    struct VisualNode {
        QPointF position;
        QPointF velocity;
        QSizeF size;
    };

    QTransform worldTransform() const
    {
        QTransform transform;
        transform.translate(width() / 2.0 + pan_.x(),
                            height() / 2.0 + pan_.y());
        transform.scale(zoom_, zoom_);
        return transform;
    }

    QPointF toWorld(const QPointF& point) const
    {
        bool invertible = false;
        const QTransform inverse = worldTransform().inverted(&invertible);
        return invertible ? inverse.map(point) : point;
    }

    int nodeAt(const QPointF& screenPoint) const
    {
        const QPointF point = toWorld(screenPoint);
        for (int index = static_cast<int>(visual_.size()) - 1;
             index >= 0; --index) {
            const VisualNode& node = visual_[static_cast<std::size_t>(index)];
            const QRectF bounds(node.position.x() - node.size.width() / 2.0,
                                node.position.y() - node.size.height() / 2.0,
                                node.size.width(), node.size.height());
            if (bounds.contains(point)) return index;
        }
        return -1;
    }

    TransitionGraphRoute routeFor(int node) const
    {
        const auto found = routeCache_.find(node);
        if (found != routeCache_.end()) return found->second;
        TransitionGraphRoute route = longestTransitionGraphRoute(graph_, node);
        routeCache_.emplace(node, route);
        return route;
    }

    TransitionGraphRoute hoveredRoute() const
    {
        return hoveredNode_ >= 0 ? routeFor(hoveredNode_)
                                 : TransitionGraphRoute();
    }

    QString routeDescription(const TransitionGraphRoute& route) const
    {
        const QString heading = route.completeSearch
            ? tr("Longest route") : tr("Best route found");
        return tr("%1 · %2 to end")
            .arg(heading, routeDurationText(route.durationSeconds));
    }

    void drawRouteCard(QPainter& painter,
                       const TransitionGraphRoute& route) const
    {
        const int cardHeight = 46;
        const QRect card(12, height() - cardHeight - 12,
                         std::max(10, width() - 24), cardHeight);
        painter.setPen(QPen(QColor(53, 200, 232, 170), 1));
        painter.setBrush(QColor(20, 27, 33, 235));
        painter.drawRoundedRect(card, 7, 7);
        painter.setPen(QColor(233, 239, 245));
        painter.drawText(card.adjusted(12, 7, -12, -7),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         routeDescription(route));
    }

    void changeZoom(double requested, const QPointF& anchor, bool notify)
    {
        const QPointF before = toWorld(anchor);
        zoom_ = std::clamp(requested, 0.35, 2.5);
        const QPointF afterScreen = worldTransform().map(before);
        pan_ += anchor - afterScreen;
        const int percent = static_cast<int>(std::lround(zoom_ * 100.0));
        setProperty("zoomPercent", percent);
        if (notify && zoomChanged_) zoomChanged_(percent);
        update();
    }

    void stepSimulation()
    {
        if (visual_.size() < 2 || !isVisible()) return;
        std::vector<QPointF> forces(visual_.size());
        for (std::size_t left = 0; left < visual_.size(); ++left) {
            forces[left] -= visual_[left].position * 0.00035;
            for (std::size_t right = left + 1;
                 right < visual_.size(); ++right) {
                QPointF delta = visual_[left].position -
                                visual_[right].position;
                double distance = std::hypot(delta.x(), delta.y());
                if (distance < 0.1) {
                    delta = QPointF(1.0, 0.0);
                    distance = 1.0;
                }
                const QPointF direction = delta / distance;
                const double desired =
                    (visual_[left].size.width() +
                     visual_[right].size.width()) / 2.0 + 48.0;
                const double strength = 6500.0 /
                    std::max(400.0, distance * distance) +
                    std::max(0.0, desired - distance) * 0.035;
                forces[left] += direction * strength;
                forces[right] -= direction * strength;
            }
        }
        for (const TransitionGraphEdge& edge : graph_.edges) {
            if (edge.from < 0 || edge.to < 0 ||
                edge.from >= static_cast<int>(visual_.size()) ||
                edge.to >= static_cast<int>(visual_.size()))
                continue;
            QPointF delta = visual_[static_cast<std::size_t>(edge.to)].position -
                            visual_[static_cast<std::size_t>(edge.from)].position;
            const double distance = std::max(1.0,
                std::hypot(delta.x(), delta.y()));
            const QPointF spring = delta / distance *
                                   ((distance - 230.0) * 0.003);
            forces[static_cast<std::size_t>(edge.from)] += spring;
            forces[static_cast<std::size_t>(edge.to)] -= spring;
        }

        for (std::size_t index = 0; index < visual_.size(); ++index) {
            if (static_cast<int>(index) == draggedNode_) continue;
            visual_[index].velocity =
                (visual_[index].velocity + forces[index]) * 0.86;
            const double speed = std::hypot(visual_[index].velocity.x(),
                                            visual_[index].velocity.y());
            if (speed > 7.0) visual_[index].velocity *= 7.0 / speed;
            visual_[index].position += visual_[index].velocity;
        }
        update();
    }

    TransitionGraph graph_;
    std::vector<VisualNode> visual_;
    mutable std::map<int, TransitionGraphRoute> routeCache_;
    QTimer simulation_;
    std::function<void(int)> zoomChanged_;
    std::array<int, 2> currentDeckNodes_ {-1, -1};
    QPointF pan_;
    QPointF lastPointer_;
    double zoom_ = 1.0;
    int hoveredNode_ = -1;
    int draggedNode_ = -1;
    bool panning_ = false;
};

TransitionGraphWindow::TransitionGraphWindow(TransitionStore* store,
                                             AudioEngine* engine,
                                             QWidget* parent)
    : QMainWindow(parent, Qt::Window), store_(store), engine_(engine)
{
    setObjectName(QStringLiteral("transitionGraphWindow"));
    setWindowTitle(tr("Transition Set Graph"));
    resize(980, 680);
    setMinimumSize(640, 460);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* zoomBar = new QWidget(central);
    zoomBar->setObjectName(QStringLiteral("transitionGraphZoomBar"));
    auto* zoomLayout = new QHBoxLayout(zoomBar);
    zoomLayout->setContentsMargins(12, 6, 12, 6);
    zoomLayout->setSpacing(8);
    zoomLayout->addWidget(new QLabel(tr("Zoom"), zoomBar));
    zoomSlider_ = new QSlider(Qt::Horizontal, zoomBar);
    zoomSlider_->setObjectName(QStringLiteral("transitionGraphZoomSlider"));
    zoomSlider_->setRange(35, 250);
    zoomSlider_->setSingleStep(5);
    zoomSlider_->setPageStep(10);
    zoomSlider_->setValue(100);
    zoomSlider_->setMaximumWidth(260);
    zoomLayout->addWidget(zoomSlider_, 1);
    auto* zoomValue = new QLabel(tr("100%"), zoomBar);
    zoomValue->setObjectName(QStringLiteral("transitionGraphZoomValue"));
    zoomValue->setMinimumWidth(42);
    zoomLayout->addWidget(zoomValue);
    zoomLayout->addStretch(1);
    layout->addWidget(zoomBar);

    canvas_ = new Canvas(central);
    layout->addWidget(canvas_, 1);
    setCentralWidget(central);
    connect(zoomSlider_, &QSlider::valueChanged, this,
            [this, zoomValue](int value) {
                zoomValue->setText(tr("%1%").arg(value));
                canvas_->setZoomPercent(value);
            });
    canvas_->setZoomChangedCallback(
        [this, zoomValue](int value) {
            const QSignalBlocker blocker(zoomSlider_);
            zoomSlider_->setValue(value);
            zoomValue->setText(tr("%1%").arg(value));
        });
    auto* help = new QLabel(
        tr("Hover a song to highlight its longest route; estimated time appears below"),
        this);
    statusBar()->addWidget(help, 1);
    if (store_)
        connect(store_, &TransitionStore::changed, this,
                &TransitionGraphWindow::refreshGraph);
    auto* deckPoll = new QTimer(this);
    deckPoll->setInterval(200);
    connect(deckPoll, &QTimer::timeout, this,
            &TransitionGraphWindow::refreshDeckHighlights);
    deckPoll->start();
    refreshGraph();
}

void TransitionGraphWindow::refreshGraph()
{
    canvas_->setGraph(buildTransitionGraph(
        store_ ? store_->all() : std::vector<GvtFile>()));
    refreshDeckHighlights();
}

void TransitionGraphWindow::refreshDeckHighlights()
{
    if (!canvas_ || !store_ || !engine_ || !isVisible()) return;

    const TransitionGraph& graph = canvas_->graph();
    const std::vector<GvtFile>& files = store_->all();
    const auto matchingNode = [&](const TrackDataPtr& track) {
        if (!track) return -1;
        for (const TransitionGraphEdge& edge : graph.edges) {
            const auto source = std::find_if(
                files.begin(), files.end(), [&edge](const GvtFile& file) {
                    return (!edge.filePath.isEmpty() &&
                            file.filePath == edge.filePath) ||
                           (edge.filePath.isEmpty() &&
                            !edge.transitionId.isEmpty() &&
                            file.id == edge.transitionId);
                });
            if (source == files.end()) continue;
            if (store_->matchesEndpoint(*source, true, *track))
                return edge.from;
            if (store_->matchesEndpoint(*source, false, *track))
                return edge.to;
        }
        return -1;
    };

    canvas_->setCurrentDeckNodes(
        matchingNode(engine_->deck(0).track()),
        matchingNode(engine_->deck(1).track()));
}

} // namespace gvt
