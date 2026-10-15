import urllib.request, json, ssl

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

login_url = 'https://c7.hust-2slab.org/api/auth/login'
req = urllib.request.Request(
    login_url, 
    data=json.dumps({'username': 'tenant@thingsboard.org', 'password': 'tenant'}).encode(), 
    headers={'Content-Type': 'application/json', 'User-Agent': 'Mozilla/5.0'}
)

try:
    with urllib.request.urlopen(req, context=ctx) as resp:
        token = json.loads(resp.read().decode())['token']
        print('[+] Logged in to ThingsBoard REST API!')
        
        dev_req = urllib.request.Request(
            'https://c7.hust-2slab.org/api/tenant/devices?pageSize=100&page=0', 
            headers={'X-Authorization': f'Bearer {token}', 'User-Agent': 'Mozilla/5.0'}
        )
        with urllib.request.urlopen(dev_req, context=ctx) as dev_resp:
            devs = json.loads(dev_resp.read().decode())['data']
            print(f'[+] Found {len(devs)} devices in Tenant:')
            for d in devs:
                dev_id = d['id']['id']
                dev_name = d['name']
                
                # Check SERVER_SCOPE attributes
                attr_req = urllib.request.Request(
                    f'https://c7.hust-2slab.org/api/plugins/telemetry/DEVICE/{dev_id}/values/attributes/SERVER_SCOPE?keys=connected,patientName',
                    headers={'X-Authorization': f'Bearer {token}', 'User-Agent': 'Mozilla/5.0'}
                )
                conn_val = 'Unknown'
                p_name = ''
                try:
                    with urllib.request.urlopen(attr_req, context=ctx) as attr_resp:
                        attrs = json.loads(attr_resp.read().decode())
                        for a in attrs:
                            if a['key'] == 'connected':
                                conn_val = a['value']
                            if a['key'] == 'patientName':
                                p_name = a['value']
                except Exception as ex:
                    conn_val = f"Err({ex})"
                    
                print(f"  • Name: {dev_name:<20} | ID: {dev_id} | Connected: {str(conn_val):<7} | Patient: {p_name}")
                    
except Exception as e:
    print('[-] Error:', e)
