// ===============================
// CONFIGURAÇÃO DO FIREBASE
// ===============================
const firebaseConfig = {
  apiKey: "AIzaSyAlRIiLVigM39ELJ8bXJUIrcIzrviCAHFg",
  authDomain: "automacaositiommcapituva.firebaseapp.com",
  databaseURL: "https://automacaositiommcapituva-default-rtdb.firebaseio.com",
  projectId: "automacaositiommcapituva",
  storageBucket: "automacaositiommcapituva.appspot.com",
  messagingSenderId: "xxxxxxxxxxxx",
  appId: "1:xxxxxxxxxxxx:web:xxxxxxxxxxxx"
};

firebase.initializeApp(firebaseConfig);
const db = firebase.database();

// ===============================
// ELEMENTOS
// ===============================
const ultimaLabel = document.getElementById("ultima");
const tabelaBody = document.querySelector("#tabela tbody");

// ===============================
// GRÁFICO
// ===============================
const ctx = document.getElementById("chart");
let chart = new Chart(ctx, {
    type: "line",
    data: {
        labels: [],
        datasets: [{
            label: "Volume (l)",
            data: [],
            borderWidth: 2,
        }]
    },
    options: {
        responsive: true
    }
});

// ===============================
// FUNÇÃO PARA CARREGAR DADOS
// ===============================
function carregarDados() {
    db.ref("historico").once("value").then(snapshot => {
        const dados = snapshot.val();
        if (!dados) return;

        const chaves = Object.keys(dados);

        // Limpar tabela e gráfico
        tabelaBody.innerHTML = "";
        chart.data.labels = [];
        chart.data.datasets[0].data = [];

        let ultima = null;

        chaves.forEach(key => {
            const item = dados[key];

            // Última leitura
            ultima = item;

            // Preenche tabela
            const tr = document.createElement("tr");
            tr.innerHTML = `
                <td>${item.timestamp}</td>
                <td>${item.volume}</td>
		<td>${item.tempInterna}</td>
		<td>${item.umidadeInterna}</td>
            `;
            tabelaBody.appendChild(tr);

            // Preenche gráfico
            chart.data.labels.push(item.timestamp);
            chart.data.datasets[0].data.push(item.volume);
        });

        chart.update();

        ultimaLabel.textContent = "teste";
	ultimaLabel.update();    
        // Atualizar última leitura no topo
        ultimaLabel.textContent = 
            `Última leitura: ${ultima.timestamp} → ${ultima.volume} l → ${ultima.tempInterna} C →  ${ultima.umidadeInterna} %`;
    });
}

// ===============================
// AUTO-REFRESH A CADA 1 MINUTO
// ===============================
setInterval(carregarDados, 300000);

// Primeira carga
carregarDados();

