package com.ac3forge.shield

import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView

/**
 * A small, D-pad-navigable settings panel.
 *
 * Every control in this app used to be an undocumented keypress: the hints bar
 * lists them, but a hints bar is a reference for someone who already knows,
 * not a way to find out what exists. Several of the newer ones - the phone
 * remote in particular - are undiscoverable without it, since nothing on
 * screen would otherwise say they are there.
 *
 * Focus is handled by hand rather than with Android's own focus system. The
 * dashboard behind this is a set of custom `View`s with nothing focusable in
 * them, so there is no focus order to join; a selected index and a redraw is
 * the whole mechanism, and it keeps every key this panel consumes in one
 * obvious place ([onKey]) instead of spread across focus listeners.
 */
class SettingsOverlay(private val context: Context) {

    /**
     * One row. [value] is read every time the panel redraws rather than
     * cached, so a row always shows the live state - including state changed
     * by the physical keys while the panel is open.
     */
    class Item(
        val title: String,
        val value: () -> String,
        val detail: (() -> String?)? = null,
        /** delta is -1 / +1 for left/right, 0 for centre/select. */
        val onChange: (Int) -> Unit,
    )

    private val rows = ArrayList<TextView>()
    private var items: List<Item> = emptyList()
    private var selected = 0

    var isOpen: Boolean = false
        private set

    private val list = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(48, 40, 48, 40)
        background = GradientDrawable().apply {
            cornerRadius = Theme.cornerRadiusLarge
            setColor(Theme.colorSurface)
            setStroke(3, Theme.colorSurfaceBorder)
        }
    }

    /** Full-screen scrim plus the panel, added once by MainActivity. */
    val view: View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        // Not fully opaque: the room view keeps moving behind this, which is
        // the point of a settings panel in a demo that never stops playing.
        setBackgroundColor(0xE0000000.toInt())
        visibility = View.GONE
        addView(TextView(context).apply {
            text = "Settings"
            textSize = 22f
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0.14f
            setTextColor(Theme.colorAccent)
            gravity = Gravity.CENTER
            setPadding(0, 0, 0, 24)
        })
        addView(list)
        addView(TextView(context).apply {
            text = "D-pad up/down to choose  ·  left/right or centre to change  ·  BACK to close"
            textSize = 14f
            setTextColor(Theme.colorTextMuted)
            gravity = Gravity.CENTER
            setPadding(0, 26, 0, 0)
        })
    }

    fun setItems(newItems: List<Item>) {
        items = newItems
        list.removeAllViews()
        rows.clear()
        // One row per item, by count only - content is populated from `items` by
        // index in refresh(), not from this loop.
        repeat(newItems.size) {
            val row = TextView(context).apply {
                textSize = 19f
                setPadding(28, 18, 28, 18)
                setLineSpacing(4f, 1f)
            }
            rows.add(row)
            list.addView(
                row,
                LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                ),
            )
        }
        if (selected >= newItems.size) selected = 0
        refresh()
    }

    fun open() {
        if (isOpen) return
        isOpen = true
        selected = 0
        refresh()
        view.visibility = View.VISIBLE
    }

    fun close() {
        if (!isOpen) return
        isOpen = false
        view.visibility = View.GONE
    }

    /** Returns true if this panel consumed the key. Only ever while open. */
    fun onKey(keyCode: Int): Boolean {
        if (!isOpen) return false
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> {
                if (items.isNotEmpty()) selected = (selected - 1 + items.size) % items.size
                refresh()
            }
            KeyEvent.KEYCODE_DPAD_DOWN -> {
                if (items.isNotEmpty()) selected = (selected + 1) % items.size
                refresh()
            }
            KeyEvent.KEYCODE_DPAD_LEFT -> change(-1)
            KeyEvent.KEYCODE_DPAD_RIGHT -> change(1)
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BUTTON_A -> change(0)
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_MENU -> close()
            else -> return false
        }
        return true
    }

    private fun change(delta: Int) {
        items.getOrNull(selected)?.onChange?.invoke(delta)
        // Re-read every row, not just the changed one: toggling the phone
        // remote changes its own row's detail line, and a scene change moves
        // more than the row that caused it.
        refresh()
    }

    /** Repaints every row from live state. Cheap - a handful of TextViews. */
    fun refresh() {
        for ((i, item) in items.withIndex()) {
            val row = rows.getOrNull(i) ?: continue
            val isSelected = i == selected
            val detail = item.detail?.invoke()
            row.text = buildString {
                append(if (isSelected) "▸  " else "    ")
                append(item.title)
                append("      ")
                append(item.value())
                if (detail != null) {
                    append('\n')
                    append("        ")
                    append(detail)
                }
            }
            row.setTextColor(if (isSelected) Theme.colorTextPrimary else Theme.colorTextSecondary)
            row.setBackgroundColor(if (isSelected) Theme.colorAccentDim else 0x00000000)
        }
    }
}
