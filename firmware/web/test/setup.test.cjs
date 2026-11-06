const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const path=require('node:path');
const script=fs.readFileSync(path.join(__dirname,'../setup.html'),'utf8').split('<script>')[1].split('</script>')[0];
function harness() {
 const elements=new Map(),requests=[],timers=new Map();let timerId=0;
 const element=()=>({textContent:'',disabled:false,value:'',children:[],replaceChildren(...c){this.children=c;}});
 const get=id=>{if(!elements.has(id))elements.set(id,element());return elements.get(id);};
 const ctx=vm.createContext({document:{getElementById:get,createElement:element,addEventListener(){},hidden:false},AbortController,
  setTimeout:fn=>{timers.set(++timerId,fn);return timerId;},clearTimeout:id=>timers.delete(id),setInterval(){},
  fetch:(url,options)=>new Promise((resolve,reject)=>{requests.push({url,options,resolve,reject});options.signal.addEventListener('abort',()=>reject(Error('timeout')));})});
 vm.runInContext(script,ctx);
 return {get,ctx,requests,timers,rows:()=>get('st').children.map(x=>x.textContent).join(' | '),
  respond:(request,data)=>request.resolve({ok:true,json:async()=>data}),poll:()=>vm.runInContext('status()',ctx)};
}
const state=(extra={})=>({wifi:{connected:true,ssid:'test'},tunnel_up:true,logged_in:true,has_credentials:true,muted:false,wake_ready:true,wake_detections:0,last_error:'',state:'in_call',call_active:true,call_connected:true,...extra});
const flush=async()=>{await new Promise(setImmediate);await new Promise(setImmediate);};
(async()=>{
 const h=harness();h.respond(h.requests.shift(),state());await flush();
 assert(h.rows().includes('Call connected'));assert(h.get('wake').disabled);assert(!h.get('hangup').disabled);
 const old=h.poll();const a=h.requests.shift();const newer=h.poll();const b=h.requests.shift();
 h.respond(b,state({state:'idle',call_active:false,call_connected:false}));await newer;
 h.respond(a,state());await old;
 assert(h.rows().includes('Idle — call ended'));assert(!h.rows().includes('Call connected'));
 const lost=h.poll();h.respond(h.requests.shift(),state({call_connected:false}));await lost;
 assert(h.rows().includes('Connection lost'));assert(!h.rows().includes('Call connected'));
 const paused=h.poll();h.respond(h.requests.shift(),state({remote_state:'halted'}));await paused;
 assert(h.rows().includes('Paused — call still connected'));
 const unreachable=h.poll();h.requests.shift().reject(Error('network'));await unreachable;
 assert(h.rows().includes('Unknown — status unavailable'));assert(!h.rows().includes('Call connected'));assert(h.get('wake').disabled);
 const recovered=h.poll();h.respond(h.requests.shift(),state());await recovered;
 const hangup=h.get('hangup').onclick();const post=h.requests.shift();
 assert.equal(post.url,'/api/control');assert.equal(JSON.parse(post.options.body).action,'hangup');
 h.respond(post,{ok:true});await flush();
 h.respond(h.requests.shift(),state({state:'idle',call_active:true,call_connected:false}));await hangup;
 assert(h.rows().includes('Ending conversation'));
 const ended=h.poll();h.respond(h.requests.shift(),state({state:'idle',call_active:false,call_connected:false}));await ended;
 assert(h.rows().includes('Idle — call ended'));assert.equal(h.get('msg').textContent,'Call ended.');assert(h.get('hangup').disabled);
 const timeout=h.poll();h.requests.shift();for(const fn of h.timers.values())fn();await timeout;
 assert(h.rows().includes('Unknown — status unavailable'));
 console.log('Setup UI: stale responses, physical connection status, paused state, network failure, teardown, and timeout PASS');
})().catch(e=>{console.error(e);process.exitCode=1;});
