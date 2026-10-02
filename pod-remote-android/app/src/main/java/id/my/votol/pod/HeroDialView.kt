package id.my.votol.pod

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.graphics.SweepGradient
import android.util.AttributeSet
import android.view.View

/**
 * Cyber-industrial hero dial (from the design kit mockups):
 *  - ambient radial glow behind the ring (state color, low alpha)
 *  - outer tick ring, 60 ticks (every 5th longer) in border color
 *  - 240-degree sweep-gradient arc in the state color, round caps
 *  - solid center disc (elevated surface) with a hairline border,
 *    leaving room for an overlaid icon
 *
 * State color is pushed in from the activity so it follows the
 * DayNight palette: set DialColor(color, tickColor, discColor).
 */
class HeroDialView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null, defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private var primary = Color.parseColor("#40F0B6")
    private var tickColor = Color.parseColor("#2B3A43")
    private var discColor = Color.parseColor("#121B22")

    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val tickPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        strokeCap = Paint.Cap.ROUND
    }
    private val arcPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
    }
    private val discPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val discBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.5f
    }

    fun setDialColors(primary: Int, tick: Int, disc: Int) {
        this.primary = primary
        this.tickColor = tick
        this.discColor = disc
        rebuildShaders()
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        rebuildShaders()
    }

    private fun rebuildShaders() {
        if (width == 0 || height == 0) return
        val cx = width / 2f
        val cy = height / 2f
        val r = minOf(cx, cy)

        glowPaint.shader = RadialGradient(
            cx, cy, r,
            intArrayOf(
                withAlpha(primary, 0x38),
                withAlpha(primary, 0x18),
                Color.TRANSPARENT,
            ),
            floatArrayOf(0.34f, 0.60f, 1.0f),
            Shader.TileMode.CLAMP
        )

        arcPaint.shader = SweepGradient(
            cx, cy,
            intArrayOf(
                withAlpha(primary, 0x00),
                withAlpha(primary, 0xFF),
                withAlpha(primary, 0xE6),
                withAlpha(primary, 0x00),
            ),
            floatArrayOf(0.00f, 0.6667f, 0.9000f, 1.0f)
        )
    }

    private fun withAlpha(c: Int, a: Int) = (c and 0x00FFFFFF) or (a shl 24)

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val cx = width / 2f
        val cy = height / 2f
        val rOuter = minOf(cx, cy) - 3f
        val rArc = rOuter - 7f
        val rTickIn = rOuter - 9f
        val rDisc = rOuter * 0.62f

        // ambient glow
        canvas.drawCircle(cx, cy, rOuter, glowPaint)

        // tick ring: 60 ticks, every 5th longer
        tickPaint.color = tickColor
        for (i in 0 until 60) {
            val major = i % 5 == 0
            val len = if (major) 8f else 4f
            val r1 = rTickIn - len
            val rad = Math.toRadians((i * 6.0) - 90.0)
            val cos = Math.cos(rad).toFloat()
            val sin = Math.sin(rad).toFloat()
            canvas.drawLine(
                cx + r1 * cos, cy + r1 * sin,
                cx + rTickIn * cos, cy + rTickIn * sin,
                tickPaint
            )
        }

        // gradient sweep arc: 240 degrees, starting lower-left
        arcPaint.strokeWidth = 9f
        val rect = android.graphics.RectF(cx - rArc, cy - rArc, cx + rArc, cy + rArc)
        canvas.drawArc(rect, 150f, 240f, false, arcPaint)

        // center disc
        discPaint.color = discColor
        discBorderPaint.color = tickColor
        canvas.drawCircle(cx, cy, rDisc, discPaint)
        canvas.drawCircle(cx, cy, rDisc, discBorderPaint)
    }
}
