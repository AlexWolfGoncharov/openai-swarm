/* TinyChart v1.0 — custom canvas chart, ~4KB */
(function(w){'use strict';
function TC(el,opts){
  this.c=typeof el==='string'?document.getElementById(el):el;
  this.o=Object.assign({
    bg:'transparent',grid:'#2a3a4a',line:'#00b4d8',
    fill:'rgba(0,180,216,0.12)',text:'#8899aa',
    pad:{t:20,r:20,b:42,l:52}
  },opts||{});
  this.d={l:[],v:[]};
  var me=this;
  this.c.addEventListener('mousemove',function(e){me._mv(e);});
  this.c.addEventListener('touchmove',function(e){me._tv(e);},{passive:true});
  this.c.addEventListener('mouseleave',function(){me._draw();});
}
TC.prototype={
  setData:function(labels,values){
    this.d={l:labels,v:values.map(Number)};
    this._resize();
    this._draw();
  },
  _resize:function(){
    var dpr=window.devicePixelRatio||1;
    var w=this.c.parentElement.clientWidth||300;
    this.c.width=w*dpr;
    this.c.height=260*dpr;
    this.c.style.width=w+'px';
    this.c.style.height='260px';
    this.c.getContext('2d').scale(dpr,dpr);
    this._w=w; this._h=260;
  },
  _draw:function(hx){
    var c=this.c,ctx=c.getContext('2d'),o=this.o,d=this.d,p=o.pad;
    var w=this._w||c.width,h=this._h||c.height;
    ctx.clearRect(0,0,w,h);
    if(!d.v.length)return;
    var gw=w-p.l-p.r,gh=h-p.t-p.b;
    var vals=d.v,n=vals.length;
    var mn=Math.min.apply(null,vals),mx=Math.max.apply(null,vals);
    var rng=mx-mn||1;
    var xS=n>1?gw/(n-1):gw;
    var X=function(i){return p.l+i*xS;};
    var Y=function(v){return p.t+gh-(v-mn)/rng*gh;};
    // grid lines & y-labels
    ctx.strokeStyle=o.grid; ctx.lineWidth=1;
    ctx.fillStyle=o.text; ctx.font='11px sans-serif'; ctx.textAlign='right';
    for(var gi=0;gi<=4;gi++){
      var gy=p.t+gh/4*gi;
      ctx.beginPath(); ctx.moveTo(p.l,gy); ctx.lineTo(p.l+gw,gy); ctx.stroke();
      var gv=mx-rng/4*gi;
      ctx.fillText(gv.toFixed(1)+'%',p.l-6,gy+4);
    }
    // fill
    ctx.beginPath();
    ctx.moveTo(X(0),Y(vals[0]));
    for(var i=1;i<n;i++) ctx.lineTo(X(i),Y(vals[i]));
    ctx.lineTo(X(n-1),h-p.b); ctx.lineTo(X(0),h-p.b);
    ctx.closePath(); ctx.fillStyle=o.fill; ctx.fill();
    // line
    ctx.beginPath(); ctx.strokeStyle=o.line; ctx.lineWidth=2; ctx.lineJoin='round';
    ctx.moveTo(X(0),Y(vals[0]));
    for(var j=1;j<n;j++) ctx.lineTo(X(j),Y(vals[j]));
    ctx.stroke();
    // x-labels (max 7)
    ctx.fillStyle=o.text; ctx.textAlign='center';
    var step=Math.max(1,Math.floor(n/7));
    for(var k=0;k<n;k++){
      if(k%step===0||k===n-1) ctx.fillText(d.l[k],X(k),h-p.b+16);
    }
    // hover indicator
    if(hx!==undefined){
      var idx=Math.round((hx-p.l)/xS);
      if(idx<0)idx=0; if(idx>=n)idx=n-1;
      var cx=X(idx),cy=Y(vals[idx]);
      // dashed line
      ctx.setLineDash([4,4]); ctx.strokeStyle=o.text; ctx.lineWidth=1;
      ctx.beginPath(); ctx.moveTo(cx,p.t); ctx.lineTo(cx,h-p.b); ctx.stroke();
      ctx.setLineDash([]);
      // dot
      ctx.beginPath(); ctx.arc(cx,cy,5,0,Math.PI*2);
      ctx.fillStyle=o.line; ctx.fill();
      // tooltip
      var txt=d.l[idx]+': '+vals[idx].toFixed(1)+'%';
      ctx.font='12px sans-serif';
      var tw=ctx.measureText(txt).width+16;
      var tx=cx-tw/2; if(tx<p.l)tx=p.l; if(tx+tw>w-p.r)tx=w-p.r-tw;
      var ty=cy-36;   if(ty<p.t)ty=cy+10;
      ctx.fillStyle='#1a2332';
      ctx.beginPath(); ctx.rect(tx,ty,tw,24); ctx.fill();
      ctx.fillStyle='#e0e6ed'; ctx.textAlign='left';
      ctx.fillText(txt,tx+8,ty+16);
    }
  },
  _px:function(e){var r=this.c.getBoundingClientRect();return e.clientX-r.left;},
  _mv:function(e){this._draw(this._px(e));},
  _tv:function(e){if(e.touches.length)this._draw(this._px(e.touches[0]));},
};
w.TinyChart=TC;
})(window);
