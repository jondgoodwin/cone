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
//imports.glDrawArrays
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
//imports.glGet
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
    let name = utf8decoder.decode( wmemory.subarray( name_ptr, name_ptr+name_len ) );
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

imports.date_now = function() {
    return Date.now() * 0.001;
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
    WebAssembly.instantiateStreaming(fetch('cone.wasm'), {"js": imports})
    .then(mod => {
        degToRad = mod.instance.exports.degreeToRadians;
        initScene();
        glRenderLoop();
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
    // exports['engine_window_resize']( gl.canvas.width, gl.canvas.height );
    gl.viewportWidth = gl.canvas.width;
    gl.viewportHeight = gl.canvas.height;
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

function getShader(type, src) {
    let shader = gl.createShader(type);
    gl.shaderSource(shader, src);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        alert(gl.getShaderInfoLog(shader));
        return null;
    }

    return shader;
}

function initShader(vssrc, fssrc) {
    let vertexShader = getShader(gl.VERTEX_SHADER, vssrc);
    let fragmentShader = getShader(gl.FRAGMENT_SHADER, fssrc);

    shaderProgram = gl.createProgram();
    gl.attachShader(shaderProgram, vertexShader);
    gl.attachShader(shaderProgram, fragmentShader);
    gl.linkProgram(shaderProgram);

    if (!gl.getProgramParameter(shaderProgram, gl.LINK_STATUS)) {
        alert("Could not initialise shaders");
    }

    gl.useProgram(shaderProgram);
    return shaderProgram;
}
