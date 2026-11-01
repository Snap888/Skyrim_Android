package com.tj.skyrim

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.min
import kotlin.math.max

/**
 * Native Android virtual Xbox controller. Draws the WinNative cyan/graphite
 * layout and forwards a complete virtual XInput state for every pointer event.
 */
class GamepadOverlayView(context: Context, private val submit: (Int, Int, Int, Int, Int, Int, Int) -> Unit, private val quickAction: (String) -> Unit) : View(context) {
    private val density = resources.displayMetrics.density
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 1.35f * density
        color = 0xff238b8d.toInt()
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL; color = 0xb810141b.toInt()
    }
    private val active = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL; color = 0x7030c7c5
    }
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xff35b9b6.toInt(); textAlign = Paint.Align.CENTER
        textSize = 18f * density; typeface = android.graphics.Typeface.create("sans-serif-light", 0)
    }
    private val smallLabel = Paint(label).apply { textSize = 14f * density }
    private data class Control(val name:String, val x:Float, val y:Float, val rx:Float, val ry:Float, val bit:Int = 0, val kind:Int = 0)
    // kind: 0 round button, 1 pill, 2 d-pad arm, 3 stick, 4 menu glyph
    private val controls = listOf(
        Control("LT",.13f,.115f,.046f,.055f,0x10000,1), Control("LB",.13f,.225f,.044f,.052f,0x100,1),
        Control("RT",.87f,.115f,.046f,.055f,0x20000,1), Control("RB",.87f,.225f,.044f,.052f,0x200,1),
        Control("Y",.70f,.61f,.038f,.038f,0x8000), Control("X",.65f,.72f,.038f,.038f,0x4000),
        Control("B",.75f,.72f,.038f,.038f,0x2000), Control("A",.70f,.83f,.038f,.038f,0x1000),
        Control("L3",.17f,.91f,.024f,.045f,0x40), Control("R3",.83f,.91f,.024f,.045f,0x80),
        Control("BACK",.45f,.95f,.038f,.045f,0x20,1), Control("START",.55f,.95f,.038f,.045f,0x10,1),
        Control("UP",.275f,.74f,.031f,.055f,1), Control("DOWN",.275f,.91f,.031f,.055f,2),
        Control("LEFT",.235f,.825f,.055f,.031f,4), Control("RIGHT",.315f,.825f,.055f,.031f,8),
        Control("LS",.13f,.67f,.078f,.078f,0,3), Control("RS",.87f,.67f,.078f,.078f,0,3),
        Control("QUICK SAVE",.43f,.30f,.060f,.035f,0,5), Control("QUICK LOAD",.57f,.30f,.060f,.035f,0,5)
    )
    private val pointers = HashMap<Int, Control>()
    private val prefs = context.getSharedPreferences("gamepad_overlay", Context.MODE_PRIVATE)
    private val positions = controls.associate { control ->
        control.name to Pair(
            prefs.getFloat(profileKey("x_", control.name), prefs.getFloat("x_" + control.name, control.x)),
            prefs.getFloat(profileKey("y_", control.name), prefs.getFloat("y_" + control.name, control.y))
        )
    }.toMutableMap()

    private val sizes = controls.associate { c ->
        c.name to prefs.getFloat(profileKey("size_", c.name), 1f)
    }.toMutableMap()
    private val shapes = controls.associate { c ->
        c.name to prefs.getString(profileKey("shape_", c.name),
            if (c.kind == 1 || c.kind == 5 || c.name in listOf("BACK", "START", "UP", "DOWN", "LEFT", "RIGHT")) "rounded_rect" else "circle")!!
    }.toMutableMap()

    private fun profileKey(axis: String, name: String): String =
        prefs.getString("active_profile", "Default").orEmpty() + "_" + axis + name

    /** Change the active layout without restarting SDLActivity or the native game. */
    fun reloadProfile() {
        controls.forEach { c ->
            positions[c.name] = Pair(
                prefs.getFloat(profileKey("x_", c.name), prefs.getFloat("x_" + c.name, c.x)),
                prefs.getFloat(profileKey("y_", c.name), prefs.getFloat("y_" + c.name, c.y))
            )
        }
        controls.forEach { c ->
            sizes[c.name] = prefs.getFloat(profileKey("size_", c.name), 1f)
            shapes[c.name] = prefs.getString(profileKey("shape_", c.name),
                if (c.kind == 1 || c.kind == 5 || c.name in listOf("BACK", "START", "UP", "DOWN", "LEFT", "RIGHT")) "rounded_rect" else "circle")!!
        }
        invalidate()
    }
    private var editMode = false
    private val dragging = HashMap<Int, String>()
    private val lastTouch = HashMap<Int, Pair<Float, Float>>()
    private val enabled = controls.associate { it.name to true }.toMutableMap()
    private val toggleMode = controls.associate { it.name to false }.toMutableMap()
    private val latched = HashSet<String>()

    fun controlNames(): List<String> = controls.map { it.name }
    fun controlSize(name: String): Float = sizes[name] ?: 1f
    fun controlShape(name: String): String = shapes[name] ?: "circle"
    fun configureAppearance(name: String, size: Float, shape: String) {
        if (name !in sizes) return
        val safeSize = size.coerceIn(0.6f, 1.8f)
        val safeShape = if (shape in listOf("circle", "square", "rect", "rounded_rect")) shape else "circle"
        sizes[name] = safeSize
        shapes[name] = safeShape
        prefs.edit()
            .putFloat(profileKey("size_", name), safeSize)
            .putString(profileKey("shape_", name), safeShape)
            .apply()
        invalidate()
    }
    fun isLayoutEditMode(): Boolean = editMode
    fun setLayoutEditMode(enabled: Boolean) {
        editMode = enabled
        if (!enabled) {
            dragging.clear()
            lastTouch.clear()
        }
        invalidate()
    }
    fun isControlEnabled(name: String): Boolean = enabled[name] ?: true
    fun isToggleMode(name: String): Boolean = toggleMode[name] ?: false
    fun configureControl(name: String, isEnabled: Boolean, isToggle: Boolean) {
        enabled[name] = isEnabled
        toggleMode[name] = isToggle
        if (!isEnabled) {
            latched.remove(name)
            val c = controls.firstOrNull { it.name == name }
            if (c != null) releaseControl(c)
        }
        emit()
    }
    private fun releaseControl(c: Control) {
        if (c.kind == 3) {
            axes.remove(c.name.hashCode())
            if (c.name == "LS") { leftX = 0; leftY = 0 } else { rightX = 0; rightY = 0 }
        } else if (c.bit == 0x10000) lt = 0
        else if (c.bit == 0x20000) rt = 0
        else buttons = buttons and c.bit.inv()
    }
    private val axes = HashMap<Int, Pair<Float,Float>>()
    private var buttons = 0
    private var lt = 0
    private var rt = 0
    private var leftX = 0
    private var leftY = 0
    private var rightX = 0
    private var rightY = 0

    private fun bounds(c:Control):RectF {
        val scale = min(width,height).toFloat()
        val pos = positions[c.name] ?: Pair(c.x, c.y)
        val factor = sizes[c.name] ?: 1f
        val cx = pos.first * width; val cy = pos.second * height
        return RectF(cx-c.rx*scale*factor,cy-c.ry*scale*factor,cx+c.rx*scale*factor,cy+c.ry*scale*factor)
    }
    private fun drawButtonShape(canvas: Canvas, b: RectF, name: String, paint: Paint) {
        when (shapes[name] ?: "circle") {
            "circle" -> canvas.drawCircle(b.centerX(), b.centerY(), max(b.width(), b.height()) / 2f, paint)
            "square" -> {
                val side = max(b.width(), b.height())
                canvas.drawRect(b.centerX()-side/2f, b.centerY()-side/2f,
                    b.centerX()+side/2f, b.centerY()+side/2f, paint)
            }
            "rect" -> canvas.drawRect(b, paint)
            else -> canvas.drawRoundRect(b, min(b.width(), b.height()) * 0.24f,
                min(b.width(), b.height()) * 0.24f, paint)
        }
    }
    private fun hit(x:Float,y:Float):Control? = controls.asReversed().firstOrNull { (editMode || isControlEnabled(it.name)) && bounds(it).contains(x,y) }
    private fun axis(c:Control,x:Float,y:Float):Pair<Float,Float> {
        val b=bounds(c); val nx=((x-(b.left+b.right)/2f)/(b.width()/2f)).coerceIn(-1f,1f)
        val ny=((y-(b.top+b.bottom)/2f)/(b.height()/2f)).coerceIn(-1f,1f)
        return Pair(nx,ny)
    }
    private fun emit() { submit(buttons,leftX,leftY,rightX,rightY,lt,rt); invalidate() }
    private fun setAxis(c:Control,p:Pair<Float,Float>) {
        val x=(p.first*32767).toInt(); val y=(-p.second*32767).toInt()
        if(c.name=="LS"){leftX=x;leftY=y}else{rightX=x;rightY=y}
    }
    override fun onDraw(canvas:Canvas) {
        super.onDraw(canvas)
        val scale=min(width,height).toFloat()
        controls.forEach { c ->
            if (!isControlEnabled(c.name) && !editMode) return@forEach
            val b=bounds(c)
            if(c.kind==5) {
                drawButtonShape(canvas,b,c.name,fill); drawButtonShape(canvas,b,c.name,line)
                canvas.drawText(if(c.name=="QUICK SAVE") "QSAVE" else "QLOAD",b.centerX(),b.centerY()+smallLabel.textSize*.34f,smallLabel)
            } else if(c.kind==3) {
                canvas.drawOval(b,fill); canvas.drawOval(b,line)
                val p=axes[c.name.hashCode()] ?: Pair(0f,0f)
                val cx=(b.left+b.right)/2f+p.first*b.width()*.32f
                val cy=(b.top+b.bottom)/2f+p.second*b.height()*.32f
                val knob=RectF(cx-b.width()*.30f,cy-b.height()*.30f,cx+b.width()*.30f,cy+b.height()*.30f)
                canvas.drawOval(knob,fill); canvas.drawOval(knob,line)
                canvas.drawText(c.name,b.centerX(),b.bottom+15*density,smallLabel)
            } else if(c.kind==1) {
                drawButtonShape(canvas,b,c.name,fill); drawButtonShape(canvas,b,c.name,line)
                canvas.drawText(c.name,b.centerX(),b.centerY()+label.textSize*.34f, label)
            } else if(c.name in listOf("UP","DOWN","LEFT","RIGHT")) {
                drawButtonShape(canvas,b,c.name,fill); drawButtonShape(canvas,b,c.name,line)
                val arrow=Path(); val cx=b.centerX(); val cy=b.centerY(); val d=9*density
                when(c.name){ "UP"->{arrow.moveTo(cx,cy-d);arrow.lineTo(cx-d,cy+d*.65f);arrow.lineTo(cx+d,cy+d*.65f)}
                    "DOWN"->{arrow.moveTo(cx,cy+d);arrow.lineTo(cx-d,cy-d*.65f);arrow.lineTo(cx+d,cy-d*.65f)}
                    "LEFT"->{arrow.moveTo(cx-d,cy);arrow.lineTo(cx+d*.65f,cy-d);arrow.lineTo(cx+d*.65f,cy+d)}
                    else->{arrow.moveTo(cx+d,cy);arrow.lineTo(cx-d*.65f,cy-d);arrow.lineTo(cx-d*.65f,cy+d)} }
                arrow.close(); canvas.drawPath(arrow,active)
            } else {
                drawButtonShape(canvas,b,c.name,fill); drawButtonShape(canvas,b,c.name,line)
                canvas.drawText(c.name,b.centerX(),b.centerY()+label.textSize*.34f,smallLabel)
            }
        }
        if (editMode) {
            val guide = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = 0xffffd54f.toInt()
                style = Paint.Style.STROKE
                strokeWidth = 2f * density
            }
            dragging.values.toSet().forEach { name ->
                controls.firstOrNull { it.name == name }?.let { canvas.drawRect(bounds(it), guide) }
            }
            canvas.drawText("DRAG CONTROLS - REOPEN EDITOR TO FINISH", width / 2f, 28f * density, smallLabel.apply { color = 0xffffd54f.toInt() })
        }
    }
    override fun onTouchEvent(e:MotionEvent):Boolean {
        val index=e.actionIndex; val id=e.getPointerId(index)
        when(e.actionMasked) {
            MotionEvent.ACTION_DOWN,MotionEvent.ACTION_POINTER_DOWN -> {
                if (editMode) {
                    val c = hit(e.getX(index), e.getY(index)) ?: return true
                    dragging[id] = c.name
                    lastTouch[id] = Pair(e.getX(index), e.getY(index))
                    invalidate()
                    return true
                }
                val c=hit(e.getX(index),e.getY(index)) ?: return true
                if (!isControlEnabled(c.name)) return true
                pointers[id]=c
                if(c.kind==5) quickAction(c.name)
                else if(c.kind==3) { val p=axis(c,e.getX(index),e.getY(index)); axes[c.name.hashCode()]=p; setAxis(c,p) }
                else if (toggleMode[c.name] == true) {
                    if (latched.contains(c.name)) {
                        latched.remove(c.name)
                        releaseControl(c)
                    } else {
                        latched.add(c.name)
                        if (c.bit == 0x10000) lt = 255 else if (c.bit == 0x20000) rt = 255 else buttons = buttons or c.bit
                    }
                } else if(c.bit==0x10000) lt=255 else if(c.bit==0x20000) rt=255 else buttons=buttons or c.bit
            }
            MotionEvent.ACTION_MOVE -> for(i in 0 until e.pointerCount) {
                val pid=e.getPointerId(i)
                if (editMode) {
                    val name = dragging[pid] ?: continue
                    val prev = lastTouch[pid] ?: Pair(e.getX(i), e.getY(i))
                    val old = positions[name] ?: continue
                    val control = controls.first { it.name == name }
                    val scale = min(width,height).toFloat().coerceAtLeast(1f)
                    val factor = sizes[name] ?: 1f
                    val nx = (old.first + (e.getX(i)-prev.first)/width).coerceIn(control.rx*factor*scale/width,1f-control.rx*factor*scale/width)
                    val ny = (old.second + (e.getY(i)-prev.second)/height).coerceIn(control.ry*factor*scale/height,1f-control.ry*factor*scale/height)
                    positions[name] = Pair(nx,ny)
                    lastTouch[pid] = Pair(e.getX(i),e.getY(i))
                    continue
                }
                val c=pointers[pid] ?: continue
                if(c.kind==3){val p=axis(c,e.getX(i),e.getY(i));axes[c.name.hashCode()]=p;setAxis(c,p)}
            }
            MotionEvent.ACTION_UP,MotionEvent.ACTION_POINTER_UP,MotionEvent.ACTION_CANCEL -> {
                if (editMode) {
                    val ids = if (e.actionMasked==MotionEvent.ACTION_CANCEL) dragging.keys.toList() else listOf(id)
                    ids.forEach { pid ->
                        val name=dragging.remove(pid) ?: return@forEach
                        lastTouch.remove(pid)
                        val pos=positions[name] ?: return@forEach
                        prefs.edit().putFloat(prefs.getString("active_profile", "Default")+"_x_"+name,pos.first).putFloat(prefs.getString("active_profile", "Default")+"_y_"+name,pos.second).apply()
                    }
                    invalidate()
                    return true
                }
                val ids=if(e.actionMasked==MotionEvent.ACTION_CANCEL) pointers.keys.toList() else listOf(id)
                ids.forEach { pid ->
                    val c=pointers.remove(pid) ?: return@forEach
                    if(c.kind==3){axes.remove(c.name.hashCode());if(c.name=="LS"){leftX=0;leftY=0}else{rightX=0;rightY=0}}
                    else if(c.bit==0x10000) lt=0 else if(c.bit==0x20000) rt=0 else buttons=buttons and c.bit.inv()
                }
            }
        }
        emit(); return true
    }
}
