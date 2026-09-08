#include <windows.h>
#include <tchar.h>
#include <io.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>

#define TAM 200
#define CHAVE_REGISTO TEXT("Software\\TrabSO2")
#define VALOR_NPIPE TEXT("NPIPE")
#define EVENTO_NOTIFICAR TEXT("notificar")

#define TIPO_NOVO_ALERTA 4

typedef struct {
	BYTE tipo;
	TCHAR msg[140];
	DWORD duracao;
} MSG_ALERTA;

typedef struct {
	HANDLE hEventoSair;
	HANDLE hEventoNotificar;
	HANDLE hTimer;
	CRITICAL_SECTION criticalSection;
	TCHAR nomePipe[TAM];
} DADOS_PLACAR;

void mostraMensagem(DADOS_PLACAR* dados, const TCHAR* texto) {
	EnterCriticalSection(&dados->criticalSection);
	_tprintf(TEXT("%s\n"), texto);
	LeaveCriticalSection(&dados->criticalSection);
}

void mostraAlertaComHora(DADOS_PLACAR* dados, const TCHAR* texto, DWORD duracao) {
	SYSTEMTIME st;
	GetLocalTime(&st);

	EnterCriticalSection(&dados->criticalSection);
	_tprintf(TEXT("%02d/%02d/%04d (%02d:%02d:%02d): '%s' (%lu segundos)\n"),
		st.wDay, st.wMonth, st.wYear,
		st.wHour, st.wMinute, st.wSecond,
		texto, duracao);
	LeaveCriticalSection(&dados->criticalSection);
}

void mostraMensagemComHora(DADOS_PLACAR* dados, const TCHAR* texto) {
	SYSTEMTIME st;
	GetLocalTime(&st);

	EnterCriticalSection(&dados->criticalSection);
	_tprintf(TEXT("%02d/%02d/%04d (%02d:%02d:%02d): '%s'\n"),
		st.wDay, st.wMonth, st.wYear,
		st.wHour, st.wMinute, st.wSecond,
		texto);
	LeaveCriticalSection(&dados->criticalSection);
}

int guardarPipeNoRegistry(const TCHAR* nomePipe) {
	HKEY chave;
	LONG resultado;

	resultado = RegCreateKeyEx(
		HKEY_CURRENT_USER,
		CHAVE_REGISTO,
		0,
		NULL,
		REG_OPTION_NON_VOLATILE,
		KEY_ALL_ACCESS,
		NULL,
		&chave,
		NULL
	);

	if (resultado != ERROR_SUCCESS) {
		return 0;
	}

	resultado = RegSetValueEx(
		chave,
		VALOR_NPIPE,
		0,
		REG_SZ,
		(const BYTE*)nomePipe,
		((DWORD)_tcslen(nomePipe) + 1) * sizeof(TCHAR)
	);

	RegCloseKey(chave);

	if (resultado != ERROR_SUCCESS) {
		return 0;
	}

	return 1;
}

int lerPipeDoRegistry(TCHAR* nomePipe, DWORD tam) {
	HKEY chave;
	DWORD tipo = 0;
	DWORD tamanhoBytes = tam * sizeof(TCHAR);
	LONG resultado;

	resultado = RegOpenKeyEx(
		HKEY_CURRENT_USER,
		CHAVE_REGISTO,
		0,
		KEY_READ,
		&chave
	);

	if (resultado != ERROR_SUCCESS) {
		return 0;
	}

	resultado = RegQueryValueEx(
		chave,
		VALOR_NPIPE,
		NULL,
		&tipo,
		(LPBYTE)nomePipe,
		&tamanhoBytes
	);

	RegCloseKey(chave);

	if (resultado != ERROR_SUCCESS || tipo != REG_SZ) {
		return 0;
	}

	return 1;
}

int lerAlertaDoRegistry(const TCHAR* nomeValor, MSG_ALERTA* alerta) {
	HKEY chave;
	DWORD tipo = 0;
	DWORD tamanho = sizeof(MSG_ALERTA);
	LONG resultado;

	resultado = RegCreateKeyEx(
		HKEY_CURRENT_USER,
		CHAVE_REGISTO,
		0,
		NULL,
		REG_OPTION_NON_VOLATILE,
		KEY_READ,
		NULL,
		&chave,
		NULL
	);

	if (resultado != ERROR_SUCCESS) {
		return 0;
	}

	ZeroMemory(alerta, sizeof(MSG_ALERTA));

	resultado = RegQueryValueEx(
		chave,
		nomeValor,
		NULL,
		&tipo,
		(LPBYTE)alerta,
		&tamanho
	);

	RegCloseKey(chave);

	if (resultado != ERROR_SUCCESS) {
		return 0;
	}

	if (tipo != REG_BINARY || tamanho != sizeof(MSG_ALERTA)) {
		return 0;
	}

	return 1;
}


DWORD WINAPI threadComandos(LPVOID lpParam) {
	DADOS_PLACAR* dados = (DADOS_PLACAR*)lpParam;
	TCHAR comando[50];
	int identificador;

	while (1) {
		EnterCriticalSection(&dados->criticalSection);
		_tprintf(TEXT("> "));
		LeaveCriticalSection(&dados->criticalSection);

		if (_fgetts(comando, 50, stdin) == NULL) {
			SetEvent(dados->hEventoSair);
			break;
		}

		comando[_tcscspn(comando, TEXT("\r\n"))] = '\0';

		if (_tcscmp(comando, TEXT("liga")) == 0) {
			identificador = rand() % 1000 + 1;

			EnterCriticalSection(&dados->criticalSection);
			_tprintf(TEXT("Identificador = %d\n"), identificador);
			LeaveCriticalSection(&dados->criticalSection);
		}
		else if (_tcscmp(comando, TEXT("desliga")) == 0) {
			mostraMensagem(dados, TEXT("A terminar o placar..."));
			SetEvent(dados->hEventoSair);
			break;
		}
		else if (_tcslen(comando) == 0) {
			/* não faz nada */
		}
		else {
			mostraMensagem(dados, TEXT("Comando invalido. Use: liga ou desliga"));
		}
	}

	return 0;
}

DWORD WINAPI threadAlertas(LPVOID lpParam) {
	DADOS_PLACAR* dados = (DADOS_PLACAR*)lpParam;
	HANDLE handles[3];
	MSG_ALERTA alerta;
	DWORD resultadoWait;
	BOOL timerAtivo = FALSE;

	handles[0] = dados->hEventoSair;
	handles[1] = dados->hEventoNotificar;
	handles[2] = dados->hTimer;

	while (1) {
		resultadoWait = WaitForMultipleObjects(3, handles, FALSE, INFINITE);

		if (resultadoWait == WAIT_OBJECT_0) {
			break;
		}
		else if (resultadoWait == WAIT_OBJECT_0 + 1) {
			if (lerAlertaDoRegistry(dados->nomePipe, &alerta)) {
				if (alerta.tipo == TIPO_NOVO_ALERTA) {
					LARGE_INTEGER tempo;

					if (timerAtivo) {
						CancelWaitableTimer(dados->hTimer);
						timerAtivo = FALSE;
					}

					_tprintf(TEXT("[DEBUG] tipo=%u msg='%s' duracao=%lu\n"),
						alerta.tipo, alerta.msg, alerta.duracao);

					mostraAlertaComHora(dados, alerta.msg, alerta.duracao);

					tempo.QuadPart = -((LONGLONG)alerta.duracao * 10000000LL);

					if (SetWaitableTimer(dados->hTimer, &tempo, 0, NULL, NULL, FALSE)) {
						timerAtivo = TRUE;
					}
					else {
						mostraMensagem(dados, TEXT("Erro ao ativar o waitable timer."));
					}
				}
				else {
					mostraMensagem(dados, TEXT("Tipo de alerta invalido."));
				}
			}
			else {
				mostraMensagem(dados, TEXT("Nao foi possivel ler o alerta do Registry."));
			}

			ResetEvent(dados->hEventoNotificar);
		}
		else if (resultadoWait == WAIT_OBJECT_0 + 2) {
			mostraMensagemComHora(dados, TEXT("---"));
			timerAtivo = FALSE;
		}
		else {
			mostraMensagem(dados, TEXT("Erro no WaitForMultipleObjects."));
			break;
		}
	}

	return 0;
}

int inicializaPlacar(DADOS_PLACAR* dados, int argc, TCHAR* argv[]) {
	ZeroMemory(dados, sizeof(DADOS_PLACAR));

	if (!InitializeCriticalSectionAndSpinCount(&dados->criticalSection, 1)) {
		return 0;
	}

	if (argc >= 2) {
		_tcsncpy_s(dados->nomePipe, TAM, argv[1], _TRUNCATE);

		if (!guardarPipeNoRegistry(dados->nomePipe)) {
			DeleteCriticalSection(&dados->criticalSection);
			return 0;
		}
	}
	else {
		if (!lerPipeDoRegistry(dados->nomePipe, TAM)) {
			DeleteCriticalSection(&dados->criticalSection);
			_tprintf(TEXT("[ERRO] O nome do 'NPIPE' não foi especificado (args ou registry)!\n"));
			return 0;
		}
	}

	dados->hEventoSair = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (dados->hEventoSair == NULL) {
		DeleteCriticalSection(&dados->criticalSection);
		return 0;
	}

	dados->hEventoNotificar = CreateEvent(NULL, TRUE, FALSE, EVENTO_NOTIFICAR);
	if (dados->hEventoNotificar == NULL) {
		CloseHandle(dados->hEventoSair);
		DeleteCriticalSection(&dados->criticalSection);
		return 0;
	}

	dados->hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
	if (dados->hTimer == NULL) {
		CloseHandle(dados->hEventoSair);
		CloseHandle(dados->hEventoNotificar);
		DeleteCriticalSection(&dados->criticalSection);
		return 0;
	}

	return 1;
}

void libertaPlacar(DADOS_PLACAR* dados) {
	if (dados->hTimer != NULL) {
		CancelWaitableTimer(dados->hTimer);
		CloseHandle(dados->hTimer);
	}

	if (dados->hEventoNotificar != NULL) {
		CloseHandle(dados->hEventoNotificar);
	}

	if (dados->hEventoSair != NULL) {
		CloseHandle(dados->hEventoSair);
	}

	DeleteCriticalSection(&dados->criticalSection);
}

int _tmain(int argc, TCHAR* argv[]) {
	DADOS_PLACAR dados;
	HANDLE hThreadComandos;
	HANDLE hThreadAlertas;

#ifdef UNICODE
	_setmode(_fileno(stdin), _O_WTEXT);
	_setmode(_fileno(stdout), _O_WTEXT);
	_setmode(_fileno(stderr), _O_WTEXT);
#endif

	srand((unsigned int)time(NULL));

	if (!inicializaPlacar(&dados, argc, argv)) {
		return 1;
	}

	EnterCriticalSection(&dados.criticalSection);
	_tprintf(TEXT("NamedPipe = '%s'\n"), dados.nomePipe);
	LeaveCriticalSection(&dados.criticalSection);

	hThreadComandos = CreateThread(
		NULL,
		0,
		threadComandos,
		&dados,
		0,
		NULL
	);

	hThreadAlertas = CreateThread(
		NULL,
		0,
		threadAlertas,
		&dados,
		0,
		NULL
	);

	if (hThreadComandos == NULL || hThreadAlertas == NULL) {
		_tprintf(TEXT("Erro ao criar as threads.\n"));
		SetEvent(dados.hEventoSair);

		if (hThreadComandos != NULL) CloseHandle(hThreadComandos);
		if (hThreadAlertas != NULL) CloseHandle(hThreadAlertas);

		libertaPlacar(&dados);
		return 1;
	}

	WaitForSingleObject(hThreadComandos, INFINITE);
	WaitForSingleObject(hThreadAlertas, INFINITE);

	CloseHandle(hThreadComandos);
	CloseHandle(hThreadAlertas);

	libertaPlacar(&dados);

	return 0;
}