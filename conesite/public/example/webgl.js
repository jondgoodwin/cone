// JS-to-WebAssembly bridge, since wasm can't reference JS objects or services directly.

let imports = {};  // JS functions, memory, tables to import to wasm instance
let wmemory;  // wasm linear memory, sharable between instances and JS

// --- Map JS objects to an integer id that wasm can use as a handle

let wasmObjs = [ null ];  // JS object vector. Index is wasm ID
let wasmFreeIds = [];     // List of unused id slots in wasmObjs

function wasmNewObj(obj) {
    if( wasmFreeIds.length == 0 )
    {
        wasmObjs.push( obj );
        return wasmObjs.length - 1;
    }
    else
    {
        let id = wasmFreeIds.shift();
        wasmObjs[id] = obj;
        return id;
    }
}

imports.jsFreeObj = function wasmFreeObj(id) {
    delete wasmObjs[id];
    wasmFreeIds.push( id );
}

// -- WebGL function imports --

imports.glActiveTexture = function(slot) {
    gl.activeTexture(slot);
}
imports.glAttachShader = function(program_id, shader_id) {
    gl.attachShader( wasmObjs[program_id], wasmObjs[shader_id] );
}
imports.glBindBuffer = function(target, buffer_id) {
    gl.bindBuffer(target, wasmObjs[buffer_id]);
}
//imports.glBindFramebuffer
//imports.glBindSampler
imports.glBindTexture = function(type, tex_id) {
    gl.bindTexture(type, wasmObjs[tex_id]);
}
imports.glBindVertexArray = function(vao_id) {
    gl.bindVertexArray(wasmObjs[vao_id]);
}
//imports.glBlendColor
//imports.glBlendEquationSeparate
//imports.glBlendFuncSeparate
imports.glBufferData = function(target, data, length, usage) {
    gl.bufferData(target, wmemory.subarray(data, data+length), usage);
}
imports.glClearBuffer = function(buffer, drawbuffer, r, g, b, a) {
    gl.clearBufferfv(buffer, drawbuffer, new Float32Array([r, g, b, a]));
}
imports.glClear = function(mask) {
	gl.clear(mask);
}
//imports.glColorMask
imports.glCompileShader = function(shader_id){
    gl.compileShader(wasmObjs[shader_id]);
}
imports.glCreateBuffer = function() {
    return wasmNewObj( gl.createBuffer() );
}
//imports.glCreateFramebuffer
imports.glCreateProgram = function() {
    return wasmNewObj( gl.createProgram() );
}
//imports.glCreateSampler
imports.glCreateShader = function(type) {
    return wasmNewObj( gl.createShader( type ) );
}
//imports.glCreateTexture
imports.glCreateVertexArray = function() {
    return wasmNewObj( gl.createVertexArray() );
}
imports.glCullFace = function( mode ) {
    gl.cullFace( mode );
}
imports.glDeleteBuffer = function( buffer_id ) {
    gl.deleteBuffer( wasmObjs[ buffer_id ] );
    wasmFreeObj( buffer_id );
}
//imports.glDeleteFramebuffer
imports.glDeleteProgram = function( program_id ) {
    gl.deleteProgram( wasmObjs[program_id] );
    wasmFreeObj( program_id )
}
//imports.glDeleteSampler
imports.glDeleteShader = function( shader_id ) {
    gl.deleteShader( wasmObjs[shader_id] );
    wasmFreeObj( shader_id );
}
//imports.glDeleteTexture
imports.glDeleteVertexArray = function( vao_id ) {
    gl.deleteVertexArray( wasmObjs[ vao_id ] );
    wasmFreeObj( vao_id );
}
//imports.glDepthFunc
//imports.glDepthMask
imports.glDetachShader = function( program_id, shader_id ) {
    gl.detachShader( wasmObjs[program_id], wasmObjs[shader_id] );
}
imports.glDisable = function( cap ) {
    gl.disable( cap );
}
imports.glDrawArrays = function (mode, first, count) {
	gl.drawArrays(mode, first, count);
}
//imports.glDrawBuffer
imports.glDrawElements = function( mode, count, type, offset ) {
    gl.drawElements( mode, count, type, offset );
}
imports.glEnable = function( cap ) {
    gl.enable( cap );
}
imports.glEnableVertexAttribArray = function( index ) {
    gl.enableVertexAttribArray( index );
}
//imports.glFramebufferParameteri
//imports.glFramebufferTexture
//imports.glFrontFace
//imports.glGenBuffer
//imports.glGenFramebuffer
//imports.glGenSampler
imports.glCreateTexture = function() {
    return wasmNewObj( gl.createTexture() );
}
imports.glGetAttribLocation = function( program_id, name_ptr, name_len ) {
    let name = utf8decoder.decode( wmemory.subarray( name_ptr, name_ptr+name_len ) );
    return gl.getAttribLocation( wasmObjs[program_id], name );
}
imports.glGetProgramParameter = function( program_id, param ) {
    return gl.getProgramParameter( wasmObjs[program_id], param );
}
//imports.glGetProgramInfoLog
imports.glGetShaderParameter = function( shader_id, param ){
    return gl.getShaderParameter( wasmObjs[shader_id], param );
}
//imports.glGetShaderInfoLog
imports.glGenerateMipmap = function( target ){
    gl.generateMipmap( target );
}
//imports.glGetString
imports.glGetUniformLocation = function( program_id, name_ptr, name_len ) {
    let name = utf8decoder.decode( wmemory.subarray( name_ptr, name_ptr+name_len-1 ) );
    return wasmNewObj( gl.getUniformLocation( wasmObjs[program_id], name ) );
}
imports.glLinkProgram = function( program_id ) {
    gl.linkProgram( wasmObjs[program_id] );
}
//imports.glLogicOp
imports.glShaderSource = function( shader_id, source_ptr, src_len ) {
    gl.shaderSource( wasmObjs[shader_id], buf2str( source_ptr, src_len ) );
}
imports.glTexParameterf = function( target, pname, param ) {
    gl.texParameterf( target, pname, param );
}
imports.glTexParameteri = function( target, pname, param ) {
    gl.texParameteri( target, pname, param );
}

imports.glTexStorage2D = function( target, levels, internalformat, width, height ) {
    gl.texStorage2D( target, levels, internalformat, width, height );
}
imports.glTexSubImage2D = function( target, level, xoff, yoff, width, height, format, type, data, len ) {
    gl.texSubImage2D( target, level, xoff, yoff, width, height, format, type, wmemory.slice( data, data+len ) );
}
imports.glUniform1i = function( location_id, value ) {
    gl.uniform1i( wasmObjs[location_id], value );
}
imports.glUniform3fv = function( location_id, x, y, z ) {
    gl.uniform3fv( wasmObjs[location_id], [x, y, z] );
}
imports.glUniformMatrix4fv = function( location_id, data ) {
	let nn = new Float32Array( wmemory.slice( data, data+4*16 ).buffer );
    gl.uniformMatrix4fv( wasmObjs[location_id], false, new Float32Array( wmemory.slice( data, data+4*16 ).buffer ) );
}
imports.glUseProgram = function( program_id ) {
    gl.useProgram( wasmObjs[program_id] );    
}
imports.glVertexAttribPointer = function( index, size, type, normalized, stride, offset ) {    
    gl.vertexAttribPointer( index, size, type, normalized, stride, offset );
}
imports.glViewport = function( x, y, w, h ) {    
    gl.viewport( x, y, w, h );
}

imports.glutil_printShaderInfoLog = function( shader_id ) {
    console.log( "Shader log:\n" + gl.getShaderInfoLog( wasmObjs[shader_id] ) );
}
imports.glutil_printProgramInfoLog = function( program_id ) {
    console.log( "Program log:\n" + gl.getProgramInfoLog( wasmObjs[program_id] ) );
}

imports.util_buffer_get = function( id, buf ) {    
    let file = files[id];
    let arr = wmemory.subarray( buf, buf+file.byteLength );
    arr.set( new Uint8Array( file ) );
    return true;
}

glcompile_shader = function ( type, src ) {
	let shader = gl.createShader(type);
	gl.shaderSource(shader, src);
	gl.compileShader(shader);
	if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
		alert(gl.getShaderInfoLog(shader));
		return 0;
	}
	return shader;
}

let utf8decoder = new TextDecoder( "utf-8" );
imports.glMakeShader = function ( vssrcpos, vssrclen, fssrcpos, fssrclen ) {
	let shaderProgram = gl.createProgram();
	let vssrc = utf8decoder.decode(wmemory.subarray(vssrcpos, vssrcpos+vssrclen-1));
	let vshader = glcompile_shader(gl.VERTEX_SHADER, vssrc);
	gl.attachShader(shaderProgram, vshader);
	let fssrc = utf8decoder.decode(wmemory.subarray(fssrcpos, fssrcpos+fssrclen-1));
	let fshader = glcompile_shader(gl.FRAGMENT_SHADER, fssrc);
	gl.attachShader(shaderProgram, fshader);
	gl.linkProgram(shaderProgram);

	if (!gl.getProgramParameter(shaderProgram, gl.LINK_STATUS)) {
		alert("Could not initialize shaders");
		return 0;
	}

	gl.deleteShader(vshader);
	gl.deleteShader(fshader);
	gl.useProgram(shaderProgram);
	return wasmNewObj(shaderProgram);
}

imports.glGetAttrib = function(pgm, namepos, namelen) {
	let name = utf8decoder.decode(wmemory.subarray(namepos, namepos+namelen-1));
	let attrib = gl.getAttribLocation(wasmObjs[pgm], name);
	gl.enableVertexAttribArray(attrib);
	return attrib;
}

imports.glGetAspectRatio = function() {
	return gl.canvas.clientWidth / gl.canvas.clientHeight;
}

imports.date_now = function() {
    return Date.now() * 0.001;
}

imports.sinf = function(nbr) {
	return Math.sin(nbr);
}

imports.cosf = function(nbr) {
	return Math.cos(nbr);
}

imports.sqrt = function(nbr) {
	return Math.sqrt(nbr);
}

// -- Window functions --

let gl = null; // WebGL Context
let degToRad;

// Sets up webgl context as canvas inside #webgl element nodeName
// It initializes the scene and triggers the endless render loop
window.onload = async function() {
    let owner = document.getElementById("webgl");
    let canvas = document.createElement('canvas');
    owner.appendChild(canvas);

    try {
        gl = canvas.getContext("webgl");
    } catch (e) {
    }
    if (!gl) {
        alert("Could not initialise WebGL, sorry :-(");
        return;
    }
    gl.enable(gl.DEPTH_TEST);
    gl.clearColor(0.0, 0.0, 0.0, 1.0);

    glResize();
    window.addEventListener( "resize", glResize );

    imports.memory = new WebAssembly.Memory({'initial':32});
    wmemory = new Uint8Array(imports.memory.buffer);
	imports.__indirect_function_table = new WebAssembly.Table({"initial":1024, "element":"anyfunc"});
    WebAssembly.instantiateStreaming(fetch('main.wasm'), {"env": imports})
    .then(mod => {
        degToRad = mod.instance.exports.degreeToRadians;
		initScene = mod.instance.exports.initScene;
		drawScene = mod.instance.exports.drawScene;
		updateScene = mod.instance.exports.updateScene;
        initScene();
        glRenderLoop();
    }, value => {
		console.log(value);
	});
}

// When window is resized, recalculate webgl context dimensions
function glResize(){
    let owner = document.getElementById("webgl");
    if (owner.nodeName == "BODY") {
        // let pr = window.devicePixelRatio;
        gl.canvas.width = (window.innerWidth) | 0;
        gl.canvas.height = (window.innerHeight) | 0;
    }
    else {
        gl.canvas.width = owner.offsetWidth;
        gl.canvas.height = owner.offsetHeight;
    }
	gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
}

// Call drawScene and updateScene 60fps
let lastTime = 0;
function glRenderLoop() {
    window.requestAnimationFrame(glRenderLoop);
    drawScene();
    let timeNow = new Date().getTime();
    if (lastTime != 0) {
        updateScene(timeNow - lastTime);
    }
    lastTime = timeNow;
}
