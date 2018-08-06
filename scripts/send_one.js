const request = require('request');

const ttl = 60000; /// 1 minute

function generate_key() {
    const alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    let key = "";
    for (let i = 0; i < 95; ++i) {
        const idx = Math.floor(Math.random() * (alphabet.length));
        key += alphabet[idx];
    }
    return key;
}

function send(address, message) {
    headers = {"keep-alive":false}
    
    request.post({url: 'http://127.0.0.1:5757/send_message', headers, json: {pub_key: address, message, ttl}},  (err, res) => {
        if (err) {
            console.error("ERROR: ", err.code);
        } else {
            if (res.statusCode != 200) {
                // console.error("Status code: ", res.statusCode);
            } else {
                console.log("response status:", res.status);
            }
        }
    })
}

const pubkey = generate_key();
const text = "test message";

send(pubkey, text);